#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <opal/capture_backend.hpp>
#include <opal/windows_idd_capture.hpp>

#include "../../../platform/windows/idd/Protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace opal {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t monotonic_us()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now().time_since_epoch()).count());
}

std::uint64_t qpc_capture_time_us(std::uint64_t frame_qpc, std::uint64_t callback_us,
                                  CaptureTimestampQuality& quality)
{
    quality = CaptureTimestampQuality::Estimated;
    if (frame_qpc == 0) return callback_us;
    LARGE_INTEGER now{};
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceCounter(&now) || !QueryPerformanceFrequency(&frequency) ||
        frequency.QuadPart <= 0 || now.QuadPart < 0 ||
        static_cast<std::uint64_t>(now.QuadPart) < frame_qpc)
        return callback_us;
    const auto delta = static_cast<long double>(static_cast<std::uint64_t>(now.QuadPart) - frame_qpc);
    const auto age_us = delta * 1000000.0L / static_cast<long double>(frequency.QuadPart);
    if (age_us < 0.0L || age_us > static_cast<long double>(callback_us)) return callback_us;
    quality = CaptureTimestampQuality::Exact;
    return callback_us - static_cast<std::uint64_t>(age_us);
}

template <class T>
void release_com(T*& value)
{
    if (value) value->Release();
    value = nullptr;
}

bool contains_case_insensitive(const wchar_t* value, const wchar_t* needle)
{
    if (!value || !needle || !*needle) return false;
    std::wstring haystack(value);
    std::wstring pattern(needle);
    std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towupper(c));
    });
    std::transform(pattern.begin(), pattern.end(), pattern.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towupper(c));
    });
    return haystack.find(pattern) != std::wstring::npos;
}

bool opal_display_text(const wchar_t* value)
{
    return contains_case_insensitive(value, L"OPALDISPLAY") ||
           contains_case_insensitive(value, L"OPAL VIRTUAL DISPLAY");
}

bool gdi_device_is_opal(const wchar_t* gdi_name)
{
    if (!gdi_name || !*gdi_name) return false;
    for (DWORD index = 0;; ++index) {
        DISPLAY_DEVICEW adapter{};
        adapter.cb = sizeof(adapter);
        if (!EnumDisplayDevicesW(nullptr, index, &adapter, 0)) break;
        if (_wcsicmp(adapter.DeviceName, gdi_name) != 0) continue;
        if (opal_display_text(adapter.DeviceID) || opal_display_text(adapter.DeviceString)) return true;

        DISPLAY_DEVICEW monitor{};
        monitor.cb = sizeof(monitor);
        if (EnumDisplayDevicesW(adapter.DeviceName, 0, &monitor, EDD_GET_DEVICE_INTERFACE_NAME) &&
            (opal_display_text(monitor.DeviceID) || opal_display_text(monitor.DeviceString)))
            return true;
        return false;
    }
    return false;
}

PlatformError dxgi_error(HRESULT value, std::string message, bool fallback = true)
{
    char suffix[32]{};
    std::snprintf(suffix, sizeof(suffix), " (HRESULT 0x%08lx)", static_cast<unsigned long>(value));
    return {PlatformComponent::Capture,
            value == DXGI_ERROR_UNSUPPORTED ? PlatformFailure::Unsupported : PlatformFailure::OsError,
            std::move(message) + suffix, fallback};
}

bool activate_idd_topology()
{
    for (DWORD index = 0;; ++index) {
        DISPLAY_DEVICEW display{};
        display.cb = sizeof(display);
        if (!EnumDisplayDevicesW(nullptr, index, &display, 0)) break;
        if ((display.StateFlags & DISPLAY_DEVICE_ACTIVE) == 0) continue;
        if (opal_display_text(display.DeviceID) || opal_display_text(display.DeviceString) ||
            gdi_device_is_opal(display.DeviceName))
            return true;
    }

    constexpr UINT32 flags = SDC_APPLY | SDC_TOPOLOGY_EXTEND |
                             SDC_ALLOW_CHANGES | SDC_PATH_PERSIST_IF_REQUIRED;
    return SetDisplayConfig(0, nullptr, 0, nullptr, flags) == ERROR_SUCCESS;
}

bool transient_frame_wait_error(DWORD error)
{
    switch (error) {
        case ERROR_NO_MORE_ITEMS:
        case ERROR_NO_MORE_FILES:
        case ERROR_NOT_READY:
        case ERROR_RETRY:
        case ERROR_BUSY:
        case ERROR_IO_PENDING:
        case ERROR_TIMEOUT:
            return true;
        default:
            return false;
    }
}

struct IddDxgiFrameOwner {
    IDXGIOutputDuplication* duplication = nullptr;
    ID3D11Texture2D* texture = nullptr;

    ~IddDxgiFrameOwner()
    {
        release_com(texture);
        if (duplication) {
            (void)duplication->ReleaseFrame();
            duplication->Release();
            duplication = nullptr;
        }
    }
};

class WindowsIddDxgiCaptureBackend final : public CaptureBackend {
public:
    ~WindowsIddDxgiCaptureBackend() override { stop(); }

    bool start(const StreamOptions& stream) override
    {
        return start(stream, nullptr);
    }

    bool start(const StreamOptions&, const DisplayTarget* target) override
    {
        stop();
        error_ = {};
        timestamp_quality_ = CaptureTimestampQuality::Estimated;
        if (!target || target->capture_kind != DisplayCaptureKind::WindowsIddSwapchain) {
            error_ = {PlatformComponent::Capture, PlatformFailure::InvalidState,
                      "targeted IDD DXGI capture requires the OPAL virtual display", true};
            return false;
        }

        const auto deadline = Clock::now() + std::chrono::seconds(3);
        do {
            if (open_opal_output()) {
                running_ = true;
                error_ = {};
                return true;
            }
            reset_dxgi();
            if (Clock::now() >= deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        } while (true);

        if (!error_) {
            error_ = {PlatformComponent::Capture, PlatformFailure::Unavailable,
                      "OPAL virtual display did not attach to a DXGI desktop output", true};
        }
        return false;
    }

    bool next(NativeVideoFrame& frame, int timeout_ms) override
    {
        frame = {};
        if (!running_ || !duplication_) return false;

        DXGI_OUTDUPL_FRAME_INFO info{};
        IDXGIResource* resource = nullptr;
        const HRESULT hr = duplication_->AcquireNextFrame(
            static_cast<UINT>(std::max(0, timeout_ms)), &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
        if (FAILED(hr) || !resource) {
            if (resource) resource->Release();
            error_ = dxgi_error(hr, "OPAL virtual display AcquireNextFrame failed");
            running_ = false;
            return false;
        }

        ID3D11Texture2D* texture = nullptr;
        const HRESULT texture_hr = resource->QueryInterface(
            __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
        resource->Release();
        if (FAILED(texture_hr) || !texture) {
            (void)duplication_->ReleaseFrame();
            error_ = dxgi_error(texture_hr, "OPAL virtual display frame is not a D3D11 texture");
            running_ = false;
            return false;
        }

        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (!desc.Width || !desc.Height ||
            (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
             desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)) {
            texture->Release();
            (void)duplication_->ReleaseFrame();
            error_ = {PlatformComponent::Capture, PlatformFailure::Unsupported,
                      "OPAL virtual display DXGI surface is not BGRA8", true};
            running_ = false;
            return false;
        }

        if (info.LastPresentTime.QuadPart == 0) {
            texture->Release();
            (void)duplication_->ReleaseFrame();
            return false;
        }

        const auto callback_us = monotonic_us();
        CaptureTimestampQuality quality = CaptureTimestampQuality::Estimated;
        const auto capture_us = qpc_capture_time_us(
            static_cast<std::uint64_t>(info.LastPresentTime.QuadPart), callback_us, quality);
        timestamp_quality_ = quality;

        duplication_->AddRef();
        auto owner = std::make_shared<IddDxgiFrameOwner>();
        owner->duplication = duplication_;
        owner->texture = texture;

        frame.kind = NativeVideoFrameKind::Opaque;
        frame.width = static_cast<int>(desc.Width);
        frame.height = static_cast<int>(desc.Height);
        frame.pixel_format = static_cast<std::uint32_t>(desc.Format);
        frame.capture_time_us = capture_us;
        frame.opaque = texture;
        frame.owner = std::move(owner);
        return true;
    }

    void stop() override
    {
        running_ = false;
        reset_dxgi();
        timestamp_quality_ = CaptureTimestampQuality::Estimated;
    }

    CaptureTimestampQuality timestamp_quality() const override { return timestamp_quality_; }
    std::string backend_name() const override { return "iddcx-targeted-dxgi+d3d11"; }
    PlatformError last_platform_error() const override { return error_; }

private:
    bool open_opal_output()
    {
        IDXGIFactory1* factory = nullptr;
        HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
        if (FAILED(hr) || !factory) {
            error_ = dxgi_error(hr, "DXGI factory creation failed while locating OPAL virtual display");
            return false;
        }

        IDXGIAdapter1* found_adapter = nullptr;
        IDXGIOutput1* found_output = nullptr;
        for (UINT adapter_index = 0; !found_output; ++adapter_index) {
            IDXGIAdapter1* candidate = nullptr;
            hr = factory->EnumAdapters1(adapter_index, &candidate);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr) || !candidate) continue;

            for (UINT output_index = 0;; ++output_index) {
                IDXGIOutput* base = nullptr;
                const HRESULT output_hr = candidate->EnumOutputs(output_index, &base);
                if (output_hr == DXGI_ERROR_NOT_FOUND) break;
                if (FAILED(output_hr) || !base) continue;

                DXGI_OUTPUT_DESC desc{};
                const bool matches = SUCCEEDED(base->GetDesc(&desc)) && desc.AttachedToDesktop &&
                                     gdi_device_is_opal(desc.DeviceName);
                if (matches) {
                    IDXGIOutput1* output1 = nullptr;
                    const HRESULT query_hr = base->QueryInterface(
                        __uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
                    if (SUCCEEDED(query_hr) && output1) {
                        candidate->AddRef();
                        found_adapter = candidate;
                        found_output = output1;
                    }
                }
                base->Release();
                if (found_output) break;
            }
            candidate->Release();
        }
        factory->Release();

        if (!found_adapter || !found_output) {
            release_com(found_adapter);
            release_com(found_output);
            error_ = {PlatformComponent::Capture, PlatformFailure::Unavailable,
                      "OPAL virtual display is connected but not attached to the DXGI desktop yet", true};
            return false;
        }

        adapter_ = found_adapter;
        output_ = found_output;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        constexpr D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
        };
        D3D_FEATURE_LEVEL selected{};
        hr = D3D11CreateDevice(adapter_, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                               levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                               &device_, &selected, &context_);
        if (hr == E_INVALIDARG) {
            hr = D3D11CreateDevice(adapter_, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                                   levels + 1, static_cast<UINT>(std::size(levels) - 1), D3D11_SDK_VERSION,
                                   &device_, &selected, &context_);
        }
        if (FAILED(hr) || !device_ || !context_) {
            error_ = dxgi_error(hr, "could not create D3D11 device for OPAL virtual display");
            return false;
        }

        hr = output_->DuplicateOutput(device_, &duplication_);
        if (FAILED(hr) || !duplication_) {
            error_ = dxgi_error(hr, "could not duplicate the OPAL virtual display output");
            return false;
        }
        return true;
    }

    void reset_dxgi()
    {
        release_com(duplication_);
        if (context_) context_->ClearState();
        release_com(context_);
        release_com(device_);
        release_com(output_);
        release_com(adapter_);
    }

    bool running_ = false;
    IDXGIAdapter1* adapter_ = nullptr;
    IDXGIOutput1* output_ = nullptr;
    IDXGIOutputDuplication* duplication_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    CaptureTimestampQuality timestamp_quality_ = CaptureTimestampQuality::Estimated;
    PlatformError error_{};
};

class WindowsIddCaptureBackend final : public CaptureBackend {
public:
    ~WindowsIddCaptureBackend() override { stop(); }

    bool start(const StreamOptions& stream) override
    {
        return start(stream, nullptr);
    }

    bool start(const StreamOptions& stream, const DisplayTarget* target) override
    {
        stop();
        error_ = {};
        timestamp_quality_ = CaptureTimestampQuality::Estimated;

        const bool idd_target = target && target->capture_kind == DisplayCaptureKind::WindowsIddSwapchain;
        if (idd_target) (void)activate_idd_topology();

        auto direct = std::make_unique<WindowsIddDxgiCaptureBackend>();
        if (direct->start(stream, target)) {
            timestamp_quality_ = direct->timestamp_quality();
            direct_ = std::move(direct);
            running_ = true;
            return true;
        }
        direct->stop();

        driver_ = CreateFileW(idd::kDevicePath, GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        if (driver_ == INVALID_HANDLE_VALUE) {
            driver_ = nullptr;
            set_error("OPAL indirect display device is unavailable (Win32 " +
                      std::to_string(GetLastError()) + ")");
            return false;
        }

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        D3D_FEATURE_LEVEL selected{};
        constexpr D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
        };
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                       levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                       &device_, &selected, &context_);
        if (hr == E_INVALIDARG) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels + 1, static_cast<UINT>(std::size(levels) - 1), D3D11_SDK_VERSION,
                                   &device_, &selected, &context_);
        }
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                                   levels + 1, static_cast<UINT>(std::size(levels) - 1), D3D11_SDK_VERSION,
                                   &device_, &selected, &context_);
        }
        if (FAILED(hr) || !device_ || !context_) {
            set_error("could not create D3D11 upload device for OPAL indirect display");
            stop();
            return false;
        }

        std::uint32_t expected_width = 0;
        std::uint32_t expected_height = 0;
        if (target && target->mode.width > 0 && target->mode.height > 0) {
            expected_width = static_cast<std::uint32_t>(std::min(target->mode.width, static_cast<int>(idd::kMaxWidth)));
            expected_height = static_cast<std::uint32_t>(std::min(target->mode.height, static_cast<int>(idd::kMaxHeight)));
        } else if (stream.max_width > 0 && stream.max_height > 0) {
            expected_width = static_cast<std::uint32_t>(std::min(stream.max_width, static_cast<int>(idd::kMaxWidth)));
            expected_height = static_cast<std::uint32_t>(std::min(stream.max_height, static_cast<int>(idd::kMaxHeight)));
        }
        if (!expected_width || !expected_height) {
            expected_width = idd::kMaxWidth;
            expected_height = idd::kMaxHeight;
        }
        const std::size_t frame_bytes = static_cast<std::size_t>(expected_width) *
                                        static_cast<std::size_t>(expected_height) * idd::kBytesPerPixel;
        reply_.resize(sizeof(idd::SharedFrameHeader) + frame_bytes);
        running_ = true;
        return true;
    }

    bool next(NativeVideoFrame& frame, int timeout_ms) override
    {
        frame = {};
        if (direct_) {
            const bool ok = direct_->next(frame, timeout_ms);
            timestamp_quality_ = direct_->timestamp_quality();
            if (!ok) {
                const auto direct_error = direct_->last_platform_error();
                if (direct_error) {
                    error_ = direct_error;
                    running_ = false;
                }
            }
            return ok;
        }
        if (!running_ || !driver_) return false;
        const auto deadline = Clock::now() +
                              std::chrono::milliseconds(std::max(0, timeout_ms));

        for (;;) {
            idd::FrameRequest request;
            request.last_sequence = last_sequence_;
            DWORD returned = 0;
            const BOOL ok = DeviceIoControl(driver_, idd::kIoctlGetFrame,
                                            &request, static_cast<DWORD>(sizeof(request)),
                                            reply_.data(), static_cast<DWORD>(reply_.size()),
                                            &returned, nullptr);
            if (ok) {
                if (returned == 0) {
                    if (timeout_ms <= 0 || Clock::now() >= deadline) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                if (returned < sizeof(idd::SharedFrameHeader)) {
                    set_error("OPAL indirect display driver returned a truncated frame header");
                    running_ = false;
                    return false;
                }
                const auto* header = reinterpret_cast<const idd::SharedFrameHeader*>(reply_.data());
                if (header->magic != idd::kFrameMagic || header->version != idd::kProtocolVersion ||
                    header->sequence <= last_sequence_ || header->width == 0 || header->height == 0 ||
                    header->width > idd::kMaxWidth || header->height > idd::kMaxHeight ||
                    header->stride < header->width * idd::kBytesPerPixel ||
                    header->bytes == 0 || header->bytes > idd::kMaxFrameBytes ||
                    sizeof(idd::SharedFrameHeader) + static_cast<std::size_t>(header->bytes) > returned) {
                    set_error("OPAL indirect display driver returned an invalid frame");
                    running_ = false;
                    return false;
                }

                if (!ensure_texture(header->width, header->height)) return false;
                const auto* pixels = reply_.data() + sizeof(idd::SharedFrameHeader);
                context_->UpdateSubresource(texture_, 0, nullptr, pixels, header->stride, 0);
                last_sequence_ = header->sequence;

                const auto callback_us = monotonic_us();
                CaptureTimestampQuality frame_quality = CaptureTimestampQuality::Estimated;
                const auto capture_us = qpc_capture_time_us(header->qpc, callback_us, frame_quality);
                timestamp_quality_ = frame_quality;

                texture_->AddRef();
                frame.kind = NativeVideoFrameKind::Opaque;
                frame.width = static_cast<int>(header->width);
                frame.height = static_cast<int>(header->height);
                frame.pixel_format = static_cast<std::uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM);
                frame.capture_time_us = capture_us;
                frame.opaque = texture_;
                frame.owner = std::shared_ptr<void>(texture_, [](void* value) {
                    static_cast<ID3D11Texture2D*>(value)->Release();
                });
                return true;
            }

            const DWORD win_error = GetLastError();
            if (win_error == ERROR_INSUFFICIENT_BUFFER) {
                set_error("OPAL indirect display mode exceeded its negotiated capture buffer");
                running_ = false;
                return false;
            }
            if (!transient_frame_wait_error(win_error)) {
                set_error("OPAL indirect display frame IOCTL failed (Win32 " +
                          std::to_string(win_error) + ")");
                running_ = false;
                return false;
            }
            if (timeout_ms <= 0 || Clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void stop() override
    {
        running_ = false;
        if (direct_) {
            direct_->stop();
            direct_.reset();
        }
        reply_.clear();
        release_com(texture_);
        release_com(context_);
        release_com(device_);
        if (driver_) {
            CloseHandle(driver_);
            driver_ = nullptr;
        }
        width_ = height_ = 0;
        last_sequence_ = 0;
        timestamp_quality_ = CaptureTimestampQuality::Estimated;
    }

    CaptureTimestampQuality timestamp_quality() const override
    {
        return timestamp_quality_;
    }

    std::string backend_name() const override
    {
        if (direct_) return direct_->backend_name();
        return "iddcx-device-frame+d3d11-upload";
    }

    PlatformError last_platform_error() const override { return error_; }

private:
    bool ensure_texture(std::uint32_t width, std::uint32_t height)
    {
        if (texture_ && width_ == width && height_ == height) return true;
        release_com(texture_);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        const HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &texture_);
        if (FAILED(hr) || !texture_) {
            set_error("could not create D3D11 texture for OPAL indirect display frame");
            running_ = false;
            return false;
        }
        width_ = width;
        height_ = height;
        return true;
    }

    void set_error(std::string message)
    {
        error_ = {PlatformComponent::Capture, PlatformFailure::OsError, std::move(message), true};
    }

    bool running_ = false;
    std::unique_ptr<CaptureBackend> direct_;
    HANDLE driver_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11Texture2D* texture_ = nullptr;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::int64_t last_sequence_ = 0;
    std::vector<std::uint8_t> reply_;
    CaptureTimestampQuality timestamp_quality_ = CaptureTimestampQuality::Estimated;
    PlatformError error_{};
};

}

std::unique_ptr<CaptureBackend> make_windows_idd_capture_backend()
{
    return std::make_unique<WindowsIddCaptureBackend>();
}

}
