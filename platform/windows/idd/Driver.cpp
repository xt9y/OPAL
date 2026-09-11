#include "Driver.hpp"

#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace opal::idd::driver {
namespace {

constexpr std::uint64_t kCpuCaptureLeaseMs = 250;

bool valid_mode(const DisplayModeRequest& mode)
{
    return mode.version == kProtocolVersion &&
           mode.width >= 640 && mode.width <= kMaxWidth && (mode.width & 1u) == 0 &&
           mode.height >= 480 && mode.height <= kMaxHeight && (mode.height & 1u) == 0 &&
           mode.refresh_hz >= 30 && mode.refresh_hz <= 240;
}

void fill_signal(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& info, const DisplayModeRequest& mode, bool monitor)
{
    info.totalSize.cx = info.activeSize.cx = mode.width;
    info.totalSize.cy = info.activeSize.cy = mode.height;
    info.vSyncFreq = {mode.refresh_hz, 1};
    info.hSyncFreq = {mode.refresh_hz * mode.height, 1};
    info.pixelRate = static_cast<UINT64>(mode.refresh_hz) * mode.width * mode.height;
    info.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    info.AdditionalSignalInfo.videoStandard = 255;
    info.AdditionalSignalInfo.vSyncFreqDivider = monitor ? 0 : 1;
}

IDDCX_MONITOR_MODE make_monitor_mode(const DisplayModeRequest& mode)
{
    IDDCX_MONITOR_MODE result{};
    result.Size = sizeof(result);
    result.Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
    fill_signal(result.MonitorVideoSignalInfo, mode, true);
    return result;
}

IDDCX_TARGET_MODE make_target_mode(const DisplayModeRequest& mode)
{
    IDDCX_TARGET_MODE result{};
    result.Size = sizeof(result);
    fill_signal(result.TargetVideoSignalInfo.targetVideoSignalInfo, mode, false);
    return result;
}

}

bool FrameStore::publish(ID3D11Texture2D* surface, ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (!surface || !device || !context) return false;
    D3D11_TEXTURE2D_DESC desc{};
    surface->GetDesc(&desc);
    if (!desc.Width || !desc.Height || desc.Width > kMaxWidth || desc.Height > kMaxHeight ||
        (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))
        return false;

    if (!staging_ || staging_width_ != desc.Width || staging_height_ != desc.Height || staging_format_ != desc.Format) {
        staging_.Reset();
        D3D11_TEXTURE2D_DESC staging = desc;
        staging.MipLevels = 1;
        staging.ArraySize = 1;
        staging.SampleDesc.Count = 1;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&staging, nullptr, &staging_)) || !staging_) return false;
        staging_width_ = desc.Width;
        staging_height_ = desc.Height;
        staging_format_ = desc.Format;
    }

    context->CopyResource(staging_.Get(), surface);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) return false;

    const std::uint32_t stride = desc.Width * kBytesPerPixel;
    const std::size_t frame_bytes = static_cast<std::size_t>(stride) * desc.Height;
    scratch_.resize(frame_bytes);
    const auto* source = static_cast<const std::uint8_t*>(mapped.pData);
    for (std::uint32_t y = 0; y < desc.Height; ++y)
        std::memcpy(scratch_.data() + static_cast<std::size_t>(y) * stride,
                    source + static_cast<std::size_t>(y) * mapped.RowPitch, stride);
    context->Unmap(staging_.Get(), 0);

    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    std::lock_guard<std::mutex> lock(mu_);
    pixels_.swap(scratch_);
    header_.magic = kFrameMagic;
    header_.version = kProtocolVersion;
    ++header_.sequence;
    header_.width = desc.Width;
    header_.height = desc.Height;
    header_.stride = stride;
    header_.format = static_cast<std::uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM);
    header_.qpc = static_cast<std::uint64_t>(qpc.QuadPart);
    header_.bytes = static_cast<std::uint32_t>(pixels_.size());
    return true;
}

NTSTATUS FrameStore::copy_if_new(std::int64_t last_sequence, void* output,
                                 std::size_t output_bytes, std::size_t& written)
{
    last_cpu_request_ms_.store(GetTickCount64(), std::memory_order_release);
    written = 0;
    if (!output) return STATUS_INVALID_PARAMETER;
    std::lock_guard<std::mutex> lock(mu_);
    if (header_.sequence <= last_sequence || pixels_.empty()) return STATUS_NO_MORE_ENTRIES;
    const std::size_t required = sizeof(header_) + pixels_.size();
    if (output_bytes < required) return STATUS_BUFFER_TOO_SMALL;
    std::memcpy(output, &header_, sizeof(header_));
    std::memcpy(static_cast<std::uint8_t*>(output) + sizeof(header_), pixels_.data(), pixels_.size());
    written = required;
    return STATUS_SUCCESS;
}

bool FrameStore::cpu_capture_requested() const noexcept
{
    const std::uint64_t last = last_cpu_request_ms_.load(std::memory_order_acquire);
    if (last == 0) return false;
    const std::uint64_t now = GetTickCount64();
    return now >= last && now - last <= kCpuCaptureLeaseMs;
}

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN swapchain, LUID render_adapter,
                                       HANDLE available_event, std::shared_ptr<FrameStore> frames)
    : swapchain_(swapchain), render_adapter_(render_adapter), available_event_(available_event),
      frames_(std::move(frames))
{
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this] { run(); });
}

SwapChainProcessor::~SwapChainProcessor()
{
    if (stop_event_) SetEvent(stop_event_);
    if (thread_.joinable()) thread_.join();
    if (stop_event_) CloseHandle(stop_event_);
}

void SwapChainProcessor::run()
{
    DWORD task_index = 0;
    HANDLE av_task = AvSetMmThreadCharacteristicsW(L"Distribution", &task_index);

    ComPtr<IDXGIFactory5> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIDevice> dxgi_device;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->EnumAdapterByLuid(render_adapter_, IID_PPV_ARGS(&adapter));
    if (SUCCEEDED(hr))
        hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                               &device, nullptr, &context);
    if (SUCCEEDED(hr)) hr = device.As(&dxgi_device);
    if (SUCCEEDED(hr)) {
        IDARG_IN_SWAPCHAINSETDEVICE set_device{};
        set_device.pDevice = dxgi_device.Get();
        hr = IddCxSwapChainSetDevice(swapchain_, &set_device);
    }

    while (SUCCEEDED(hr)) {
        IDARG_OUT_RELEASEANDACQUIREBUFFER buffer{};
        hr = IddCxSwapChainReleaseAndAcquireBuffer(swapchain_, &buffer);
        if (hr == E_PENDING) {
            HANDLE waits[] = {available_event_, stop_event_};
            const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 16);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_TIMEOUT) continue;
            if (wait == WAIT_OBJECT_0 + 1) break;
            hr = HRESULT_FROM_WIN32(GetLastError());
            break;
        }
        if (FAILED(hr)) break;

        ComPtr<IDXGIResource> resource;
        resource.Attach(buffer.MetaData.pSurface);
        if (frames_ && frames_->cpu_capture_requested()) {
            ComPtr<ID3D11Texture2D> texture;
            if (resource) (void)resource.As(&texture);
            if (texture) (void)frames_->publish(texture.Get(), device.Get(), context.Get());
        }
        resource.Reset();
        hr = IddCxSwapChainFinishedProcessingFrame(swapchain_);
    }

    if (swapchain_) {
        WdfObjectDelete(reinterpret_cast<WDFOBJECT>(swapchain_));
        swapchain_ = nullptr;
    }
    if (av_task) AvRevertMmThreadCharacteristics(av_task);
}

MonitorContext::MonitorContext(IDDCX_MONITOR monitor, DisplayModeRequest mode,
                               std::shared_ptr<FrameStore> frames)
    : monitor_(monitor), mode_(mode), frames_(std::move(frames)) {}
MonitorContext::~MonitorContext() { unassign(); }

void MonitorContext::assign(IDDCX_SWAPCHAIN swapchain, LUID render_adapter, HANDLE available_event)
{
    processor_.reset();
    processor_ = std::make_unique<SwapChainProcessor>(swapchain, render_adapter, available_event, frames_);
}

void MonitorContext::unassign() { processor_.reset(); }

DeviceContext::DeviceContext(WDFDEVICE device)
    : device_(device), frames_(std::make_shared<FrameStore>()) {}

DeviceContext::~DeviceContext()
{
    std::lock_guard<std::mutex> lock(mu_);
    (void)destroy_monitor_locked();
}

void DeviceContext::initialize_adapter()
{
    IDDCX_ADAPTER_CAPS caps{};
    caps.Size = sizeof(caps);
    caps.MaxMonitorsSupported = 1;
    caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
    caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
    caps.EndPointDiagnostics.pEndPointFriendlyName = L"OPAL Virtual Display";
    caps.EndPointDiagnostics.pEndPointManufacturerName = L"xt9y";
    caps.EndPointDiagnostics.pEndPointModelName = L"OPAL";
    IDDCX_ENDPOINT_VERSION version{};
    version.Size = sizeof(version);
    version.MajorVer = 1;
    caps.EndPointDiagnostics.pFirmwareVersion = &version;
    caps.EndPointDiagnostics.pHardwareVersion = &version;

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContextWrapper);
    IDARG_IN_ADAPTER_INIT input{};
    input.WdfDevice = device_;
    input.pCaps = &caps;
    input.ObjectAttributes = &attributes;
    IDARG_OUT_ADAPTER_INIT output{};
    const NTSTATUS status = IddCxAdapterInitAsync(&input, &output);
    if (!NT_SUCCESS(status)) return;

    std::lock_guard<std::mutex> lock(mu_);
    adapter_ = output.AdapterObject;
    OpalGetDeviceContext(reinterpret_cast<WDFOBJECT>(adapter_))->value = this;
}

void DeviceContext::adapter_initialized(NTSTATUS status)
{
    std::lock_guard<std::mutex> lock(mu_);
    adapter_ready_ = NT_SUCCESS(status);
}

NTSTATUS DeviceContext::create_monitor(const DisplayModeRequest& mode)
{
    if (!valid_mode(mode)) return STATUS_INVALID_PARAMETER;
    std::lock_guard<std::mutex> lock(mu_);
    if (!adapter_ready_ || !adapter_) return STATUS_DEVICE_NOT_READY;
    if (monitor_ && mode.width == mode_.width && mode.height == mode_.height &&
        mode.refresh_hz == mode_.refresh_hz) return STATUS_SUCCESS;
    if (monitor_) {
        NTSTATUS status = destroy_monitor_locked();
        if (!NT_SUCCESS(status)) return status;
    }
    return create_monitor_locked(mode);
}

NTSTATUS DeviceContext::set_mode(const DisplayModeRequest& mode)
{
    if (!valid_mode(mode)) return STATUS_INVALID_PARAMETER;
    std::lock_guard<std::mutex> lock(mu_);
    if (!adapter_ready_ || !adapter_) return STATUS_DEVICE_NOT_READY;
    if (monitor_ && mode.width == mode_.width && mode.height == mode_.height &&
        mode.refresh_hz == mode_.refresh_hz) return STATUS_SUCCESS;
    NTSTATUS status = destroy_monitor_locked();
    if (!NT_SUCCESS(status)) return status;
    return create_monitor_locked(mode);
}

NTSTATUS DeviceContext::create_monitor_locked(const DisplayModeRequest& mode)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, MonitorContextWrapper);
    attributes.EvtCleanupCallback = [](WDFOBJECT object) {
        auto* wrapper = OpalGetMonitorContext(object);
        delete wrapper->value;
        wrapper->value = nullptr;
    };

    IDDCX_MONITOR_INFO info{};
    info.Size = sizeof(info);
    info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER;
    info.ConnectorIndex = 0;
    info.MonitorDescription.Size = sizeof(info.MonitorDescription);
    info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    info.MonitorDescription.DataSize = 0;
    info.MonitorDescription.pData = nullptr;
    info.MonitorContainerId = {0xd61c0ee7, 0x64c6, 0x4dc0, {0x9d, 0x58, 0x26, 0x9f, 0x7c, 0x24, 0x91, 0xa1}};

    IDARG_IN_MONITORCREATE input{};
    input.ObjectAttributes = &attributes;
    input.pMonitorInfo = &info;
    IDARG_OUT_MONITORCREATE output{};
    NTSTATUS status = IddCxMonitorCreate(adapter_, &input, &output);
    if (!NT_SUCCESS(status)) return status;

    auto* wrapper = OpalGetMonitorContext(reinterpret_cast<WDFOBJECT>(output.MonitorObject));
    wrapper->value = new MonitorContext(output.MonitorObject, mode, frames_);
    IDARG_OUT_MONITORARRIVAL arrival{};
    status = IddCxMonitorArrival(output.MonitorObject, &arrival);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(reinterpret_cast<WDFOBJECT>(output.MonitorObject));
        return status;
    }
    monitor_ = output.MonitorObject;
    mode_ = mode;
    return STATUS_SUCCESS;
}

NTSTATUS DeviceContext::destroy_monitor()
{
    std::lock_guard<std::mutex> lock(mu_);
    return destroy_monitor_locked();
}

NTSTATUS DeviceContext::destroy_monitor_locked()
{
    if (!monitor_) return STATUS_SUCCESS;
    auto* wrapper = OpalGetMonitorContext(reinterpret_cast<WDFOBJECT>(monitor_));
    if (wrapper && wrapper->value) wrapper->value->unassign();
    const NTSTATUS status = IddCxMonitorDeparture(monitor_);
    if (!NT_SUCCESS(status)) return status;
    WdfObjectDelete(reinterpret_cast<WDFOBJECT>(monitor_));
    monitor_ = nullptr;
    mode_ = {};
    return STATUS_SUCCESS;
}

DriverStatus DeviceContext::status()
{
    std::lock_guard<std::mutex> lock(mu_);
    DriverStatus result;
    result.adapter_ready = adapter_ready_ ? 1u : 0u;
    result.monitor_active = monitor_ ? 1u : 0u;
    if (monitor_) {
        result.width = mode_.width;
        result.height = mode_.height;
        result.refresh_hz = mode_.refresh_hz;
    }
    return result;
}

}

using namespace opal::idd;
using namespace opal::idd::driver;

extern "C" DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD OpalDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY OpalDeviceD0Entry;
EVT_IDD_CX_DEVICE_IO_CONTROL OpalDeviceIoControl;
EVT_IDD_CX_ADAPTER_INIT_FINISHED OpalAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES OpalAdapterCommitModes;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION OpalParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES OpalMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES OpalMonitorQueryTargetModes;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN OpalMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN OpalMonitorUnassignSwapChain;

extern "C" BOOL WINAPI DllMain(HINSTANCE, UINT, LPVOID) { return TRUE; }

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver_object, PUNICODE_STRING registry_path)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, OpalDeviceAdd);
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    return WdfDriverCreate(driver_object, registry_path, &attributes, &config, WDF_NO_HANDLE);
}

_Use_decl_annotations_
NTSTATUS OpalDeviceAdd(WDFDRIVER driver, PWDFDEVICE_INIT init)
{
    UNREFERENCED_PARAMETER(driver);
    WDF_PNPPOWER_EVENT_CALLBACKS power;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&power);
    power.EvtDeviceD0Entry = OpalDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &power);

    IDD_CX_CLIENT_CONFIG idd_config;
    IDD_CX_CLIENT_CONFIG_INIT(&idd_config);
    idd_config.EvtIddCxDeviceIoControl = OpalDeviceIoControl;
    idd_config.EvtIddCxAdapterInitFinished = OpalAdapterInitFinished;
    idd_config.EvtIddCxParseMonitorDescription = OpalParseMonitorDescription;
    idd_config.EvtIddCxMonitorGetDefaultDescriptionModes = OpalMonitorGetDefaultModes;
    idd_config.EvtIddCxMonitorQueryTargetModes = OpalMonitorQueryTargetModes;
    idd_config.EvtIddCxAdapterCommitModes = OpalAdapterCommitModes;
    idd_config.EvtIddCxMonitorAssignSwapChain = OpalMonitorAssignSwapChain;
    idd_config.EvtIddCxMonitorUnassignSwapChain = OpalMonitorUnassignSwapChain;
    NTSTATUS status = IddCxDeviceInitConfig(init, &idd_config);
    if (!NT_SUCCESS(status)) return status;

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContextWrapper);
    attributes.EvtCleanupCallback = [](WDFOBJECT object) {
        auto* wrapper = OpalGetDeviceContext(object);
        delete wrapper->value;
        wrapper->value = nullptr;
    };
    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;

    DECLARE_CONST_UNICODE_STRING(symbolic_link, L"\\DosDevices\\Global\\OpalDisplay");
    status = WdfDeviceCreateSymbolicLink(device, &symbolic_link);
    if (!NT_SUCCESS(status)) return status;
    status = IddCxDeviceInitialize(device);
    if (!NT_SUCCESS(status)) return status;
    OpalGetDeviceContext(device)->value = new DeviceContext(device);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS OpalDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous)
{
    UNREFERENCED_PARAMETER(previous);
    auto* wrapper = OpalGetDeviceContext(device);
    if (wrapper && wrapper->value) wrapper->value->initialize_adapter();
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID OpalDeviceIoControl(WDFDEVICE device, WDFREQUEST request, size_t output_length,
                         size_t input_length, ULONG code)
{
    UNREFERENCED_PARAMETER(input_length);
    auto* wrapper = OpalGetDeviceContext(device);
    DeviceContext* context = wrapper ? wrapper->value : nullptr;
    NTSTATUS status = context ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
    std::size_t written = 0;

    if (context && code == kIoctlGetStatus) {
        void* output = nullptr; size_t size = 0;
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(DriverStatus), &output, &size);
        if (NT_SUCCESS(status)) {
            const auto current = context->status();
            std::memcpy(output, &current, sizeof(current));
            written = sizeof(current);
        }
    } else if (context && (code == kIoctlCreateMonitor || code == kIoctlSetMode)) {
        void* input = nullptr; size_t size = 0;
        status = WdfRequestRetrieveInputBuffer(request, sizeof(DisplayModeRequest), &input, &size);
        if (NT_SUCCESS(status)) {
            const auto mode = *static_cast<const DisplayModeRequest*>(input);
            status = code == kIoctlCreateMonitor ? context->create_monitor(mode) : context->set_mode(mode);
        }
    } else if (context && code == kIoctlDestroyMonitor) {
        status = context->destroy_monitor();
    } else if (context && code == kIoctlGetFrame) {
        void* input = nullptr; void* output = nullptr;
        size_t input_size = 0, output_size = 0;
        status = WdfRequestRetrieveInputBuffer(request, sizeof(FrameRequest), &input, &input_size);
        if (NT_SUCCESS(status))
            status = WdfRequestRetrieveOutputBuffer(request, sizeof(SharedFrameHeader), &output, &output_size);
        if (NT_SUCCESS(status)) {
            const auto frame_request = *static_cast<const FrameRequest*>(input);
            if (frame_request.version != kProtocolVersion) status = STATUS_REVISION_MISMATCH;
            else status = context->frames()->copy_if_new(frame_request.last_sequence, output,
                                                          std::min(output_size, output_length), written);
        }
    } else if (context) {
        status = STATUS_INVALID_DEVICE_REQUEST;
    }
    WdfRequestCompleteWithInformation(request, status, written);
}

_Use_decl_annotations_
NTSTATUS OpalAdapterInitFinished(IDDCX_ADAPTER adapter, const IDARG_IN_ADAPTER_INIT_FINISHED* input)
{
    auto* wrapper = OpalGetDeviceContext(reinterpret_cast<WDFOBJECT>(adapter));
    if (wrapper && wrapper->value) wrapper->value->adapter_initialized(input->AdapterInitStatus);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS OpalAdapterCommitModes(IDDCX_ADAPTER adapter, const IDARG_IN_COMMITMODES* input)
{
    UNREFERENCED_PARAMETER(adapter);
    UNREFERENCED_PARAMETER(input);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS OpalParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* input,
                                     IDARG_OUT_PARSEMONITORDESCRIPTION* output)
{
    UNREFERENCED_PARAMETER(input);
    output->MonitorModeBufferOutputCount = 0;
    return STATUS_NOT_SUPPORTED;
}

_Use_decl_annotations_
NTSTATUS OpalMonitorGetDefaultModes(IDDCX_MONITOR monitor,
                                    const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* input,
                                    IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* output)
{
    auto* wrapper = OpalGetMonitorContext(reinterpret_cast<WDFOBJECT>(monitor));
    if (!wrapper || !wrapper->value) return STATUS_DEVICE_NOT_READY;
    output->DefaultMonitorModeBufferOutputCount = 1;
    if (input->DefaultMonitorModeBufferInputCount == 0) return STATUS_SUCCESS;
    if (input->DefaultMonitorModeBufferInputCount < 1) return STATUS_BUFFER_TOO_SMALL;
    input->pDefaultMonitorModes[0] = make_monitor_mode(wrapper->value->mode());
    output->PreferredMonitorModeIdx = 0;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS OpalMonitorQueryTargetModes(IDDCX_MONITOR monitor,
                                     const IDARG_IN_QUERYTARGETMODES* input,
                                     IDARG_OUT_QUERYTARGETMODES* output)
{
    auto* wrapper = OpalGetMonitorContext(reinterpret_cast<WDFOBJECT>(monitor));
    if (!wrapper || !wrapper->value) return STATUS_DEVICE_NOT_READY;
    output->TargetModeBufferOutputCount = 1;
    if (input->TargetModeBufferInputCount == 0) return STATUS_SUCCESS;
    if (input->TargetModeBufferInputCount < 1) return STATUS_BUFFER_TOO_SMALL;
    input->pTargetModes[0] = make_target_mode(wrapper->value->mode());
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS OpalMonitorAssignSwapChain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN* input)
{
    auto* wrapper = OpalGetMonitorContext(reinterpret_cast<WDFOBJECT>(monitor));
    if (!wrapper || !wrapper->value) return STATUS_DEVICE_NOT_READY;
    wrapper->value->assign(input->hSwapChain, input->RenderAdapterLuid, input->hNextSurfaceAvailable);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS OpalMonitorUnassignSwapChain(IDDCX_MONITOR monitor)
{
    auto* wrapper = OpalGetMonitorContext(reinterpret_cast<WDFOBJECT>(monitor));
    if (wrapper && wrapper->value) wrapper->value->unassign();
    return STATUS_SUCCESS;
}