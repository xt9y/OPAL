#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>
#include <powrprof.h>
#include <powersetting.h>

#include <opal/display_backend.hpp>
#include <opal/windows_power_compat.hpp>

#include "../../../platform/windows/idd/Protocol.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace opal {
namespace {

enum class PhysicalSignal : int {
    Unknown = -1,
    Unavailable = 0,
    Available = 1,
};

struct DdcPowerSample {
    PhysicalSignal signal = PhysicalSignal::Unknown;
    unsigned physical_monitors = 0;
    unsigned known_on = 0;
    unsigned known_off = 0;
    unsigned unknown = 0;

    bool confirmed_single_on() const noexcept
    {
        return physical_monitors == 1 && known_on == 1 && known_off == 0 && unknown == 0;
    }
};

constexpr GUID kSessionDisplayStatus = {
    0x2b84c20e, 0xad23, 0x4ddf, {0x93, 0xdb, 0x05, 0xff, 0xbd, 0x7e, 0xfc, 0xa5}
};
constexpr BYTE kVcpPowerMode = 0xd6;
constexpr auto kHealthRefreshInterval = std::chrono::milliseconds(750);
constexpr unsigned kDdcDisappearThreshold = 4;

bool guid_equal(const GUID& a, const GUID& b)
{
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
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

bool legacy_physical_display_present()
{
    for (DWORD index = 0;; ++index) {
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        if (!EnumDisplayDevicesW(nullptr, index, &device, 0)) break;
        if ((device.StateFlags & DISPLAY_DEVICE_ACTIVE) == 0 ||
            (device.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) != 0)
            continue;
        if (opal_display_text(device.DeviceID) || opal_display_text(device.DeviceString))
            continue;
        return true;
    }
    return false;
}

bool display_path_is_opal(const DISPLAYCONFIG_PATH_INFO& path)
{
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = path.sourceInfo.adapterId;
    source.header.id = path.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS) {
        if (gdi_device_is_opal(source.viewGdiDeviceName) || opal_display_text(source.viewGdiDeviceName))
            return true;
    }

    DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
    target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    target.header.size = sizeof(target);
    target.header.adapterId = path.targetInfo.adapterId;
    target.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS) {
        if (opal_display_text(target.monitorFriendlyDeviceName) || opal_display_text(target.monitorDevicePath))
            return true;
    }
    return false;
}

PhysicalSignal ccd_physical_signal()
{
    constexpr UINT32 flags = QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE;
    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT32 path_count = 0;
        UINT32 mode_count = 0;
        LONG result = GetDisplayConfigBufferSizes(flags, &path_count, &mode_count);
        if (result != ERROR_SUCCESS) return PhysicalSignal::Unknown;

        std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
        result = QueryDisplayConfig(flags, &path_count, paths.data(), &mode_count, modes.data(), nullptr);
        if (result == ERROR_INSUFFICIENT_BUFFER) continue;
        if (result != ERROR_SUCCESS) return PhysicalSignal::Unknown;

        bool saw_physical = false;
        bool available = false;
        for (UINT32 index = 0; index < path_count; ++index) {
            const auto& path = paths[index];
            if (display_path_is_opal(path)) continue;
            saw_physical = true;
            if (path.targetInfo.targetAvailable) available = true;
        }
        if (!saw_physical) return PhysicalSignal::Unavailable;
        return available ? PhysicalSignal::Available : PhysicalSignal::Unavailable;
    }
    return PhysicalSignal::Unknown;
}

PhysicalSignal topology_physical_signal()
{
    const auto ccd = ccd_physical_signal();
    if (ccd != PhysicalSignal::Unknown) return ccd;
    return legacy_physical_display_present() ? PhysicalSignal::Available : PhysicalSignal::Unavailable;
}

BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM data)
{
    auto* monitors = reinterpret_cast<std::vector<HMONITOR>*>(data);
    if (!monitors) return FALSE;
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) && !gdi_device_is_opal(info.szDevice))
        monitors->push_back(monitor);
    return TRUE;
}

class MonitorConfigurationApi {
public:
    MonitorConfigurationApi()
    {
        module_ = LoadLibraryW(L"Dxva2.dll");
        if (!module_) return;
        get_count_ = reinterpret_cast<GetCountFn>(GetProcAddress(module_, "GetNumberOfPhysicalMonitorsFromHMONITOR"));
        get_monitors_ = reinterpret_cast<GetMonitorsFn>(GetProcAddress(module_, "GetPhysicalMonitorsFromHMONITOR"));
        destroy_monitors_ = reinterpret_cast<DestroyMonitorsFn>(GetProcAddress(module_, "DestroyPhysicalMonitors"));
        get_vcp_ = reinterpret_cast<GetVcpFn>(GetProcAddress(module_, "GetVCPFeatureAndVCPFeatureReply"));
    }

    ~MonitorConfigurationApi()
    {
        if (module_) FreeLibrary(module_);
    }

    bool valid() const noexcept
    {
        return get_count_ && get_monitors_ && destroy_monitors_ && get_vcp_;
    }

    BOOL get_count(HMONITOR monitor, LPDWORD count) const
    {
        return get_count_ ? get_count_(monitor, count) : FALSE;
    }

    BOOL get_monitors(HMONITOR monitor, DWORD count, LPPHYSICAL_MONITOR monitors) const
    {
        return get_monitors_ ? get_monitors_(monitor, count, monitors) : FALSE;
    }

    BOOL destroy(DWORD count, LPPHYSICAL_MONITOR monitors) const
    {
        return destroy_monitors_ ? destroy_monitors_(count, monitors) : FALSE;
    }

    BOOL get_vcp(HANDLE monitor, BYTE code, LPMC_VCP_CODE_TYPE type,
                 LPDWORD current, LPDWORD maximum) const
    {
        return get_vcp_ ? get_vcp_(monitor, code, type, current, maximum) : FALSE;
    }

private:
    using GetCountFn = BOOL (WINAPI*)(HMONITOR, LPDWORD);
    using GetMonitorsFn = BOOL (WINAPI*)(HMONITOR, DWORD, LPPHYSICAL_MONITOR);
    using DestroyMonitorsFn = BOOL (WINAPI*)(DWORD, LPPHYSICAL_MONITOR);
    using GetVcpFn = BOOL (WINAPI*)(HANDLE, BYTE, LPMC_VCP_CODE_TYPE, LPDWORD, LPDWORD);

    HMODULE module_ = nullptr;
    GetCountFn get_count_ = nullptr;
    GetMonitorsFn get_monitors_ = nullptr;
    DestroyMonitorsFn destroy_monitors_ = nullptr;
    GetVcpFn get_vcp_ = nullptr;
};

DdcPowerSample ddc_power_sample()
{
    DdcPowerSample sample;
    MonitorConfigurationApi api;
    if (!api.valid()) {
        sample.unknown = 1;
        return sample;
    }

    std::vector<HMONITOR> monitors;
    if (!EnumDisplayMonitors(nullptr, nullptr, collect_monitor,
                             reinterpret_cast<LPARAM>(&monitors))) {
        sample.unknown = 1;
        return sample;
    }
    if (monitors.empty()) {
        sample.unknown = 1;
        return sample;
    }

    for (HMONITOR monitor : monitors) {
        DWORD count = 0;
        if (!api.get_count(monitor, &count) || count == 0) {
            ++sample.unknown;
            continue;
        }
        sample.physical_monitors += count;
        std::vector<PHYSICAL_MONITOR> physical(count);
        if (!api.get_monitors(monitor, count, physical.data())) {
            sample.unknown += count;
            continue;
        }

        for (const auto& item : physical) {
            DWORD current = 0;
            DWORD maximum = 0;
            MC_VCP_CODE_TYPE type{};
            if (!api.get_vcp(item.hPhysicalMonitor, kVcpPowerMode, &type, &current, &maximum)) {
                ++sample.unknown;
                continue;
            }
            if (current == 0x01) ++sample.known_on;
            else if (current >= 0x02 && current <= 0x05) ++sample.known_off;
            else ++sample.unknown;
        }
        (void)api.destroy(count, physical.data());
    }

    if (sample.known_on > 0) sample.signal = PhysicalSignal::Available;
    else if (sample.known_off > 0 && sample.unknown == 0) sample.signal = PhysicalSignal::Unavailable;
    return sample;
}

class SessionDisplayPower {
public:
    SessionDisplayPower()
    {
        module_ = LoadLibraryW(L"PowrProf.dll");
        if (!module_) return;
        register_ = reinterpret_cast<RegisterFn>(GetProcAddress(module_, "PowerSettingRegisterNotification"));
        unregister_ = reinterpret_cast<UnregisterFn>(GetProcAddress(module_, "PowerSettingUnregisterNotification"));
        if (!register_ || !unregister_) return;

        parameters_.Callback = &SessionDisplayPower::callback;
        parameters_.Context = this;
        if (register_(&kSessionDisplayStatus, DEVICE_NOTIFY_CALLBACK,
                      &parameters_, &registration_) != ERROR_SUCCESS)
            registration_ = nullptr;
    }

    ~SessionDisplayPower()
    {
        if (registration_ && unregister_) (void)unregister_(registration_);
        if (module_) FreeLibrary(module_);
    }

    PhysicalSignal signal() const noexcept
    {
        return static_cast<PhysicalSignal>(state_.load(std::memory_order_acquire));
    }

private:
    using RegisterFn = DWORD (WINAPI*)(LPCGUID, DWORD, HANDLE, PHPOWERNOTIFY);
    using UnregisterFn = DWORD (WINAPI*)(HPOWERNOTIFY);

    static ULONG CALLBACK callback(PVOID context, ULONG type, PVOID setting)
    {
        if (!context || type != PBT_POWERSETTINGCHANGE || !setting) return ERROR_SUCCESS;
        const auto* broadcast = static_cast<const POWERBROADCAST_SETTING*>(setting);
        if (!guid_equal(broadcast->PowerSetting, kSessionDisplayStatus) ||
            broadcast->DataLength < sizeof(DWORD))
            return ERROR_SUCCESS;

        DWORD value = 0;
        std::memcpy(&value, broadcast->Data, sizeof(value));
        auto* self = static_cast<SessionDisplayPower*>(context);
        if (value == 0)
            self->state_.store(static_cast<int>(PhysicalSignal::Unavailable), std::memory_order_release);
        else if (value == 1 || value == 2)
            self->state_.store(static_cast<int>(PhysicalSignal::Available), std::memory_order_release);
        else
            self->state_.store(static_cast<int>(PhysicalSignal::Unknown), std::memory_order_release);
        return ERROR_SUCCESS;
    }

    HMODULE module_ = nullptr;
    RegisterFn register_ = nullptr;
    UnregisterFn unregister_ = nullptr;
    DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS parameters_{};
    HPOWERNOTIFY registration_ = nullptr;
    std::atomic<int> state_{static_cast<int>(PhysicalSignal::Unknown)};
};

bool physical_usable(PhysicalSignal topology, PhysicalSignal session, PhysicalSignal ddc)
{
    if (topology == PhysicalSignal::Unavailable ||
        session == PhysicalSignal::Unavailable ||
        ddc == PhysicalSignal::Unavailable)
        return false;
    return true;
}

class PhysicalHealthMonitor {
public:
    PhysicalHealthMonitor() = default;
    ~PhysicalHealthMonitor() { stop(); }

    bool prepare()
    {
        stop();
        const auto topology = topology_physical_signal();
        topology_.store(static_cast<int>(topology), std::memory_order_release);
        const auto sample = topology == PhysicalSignal::Unavailable ? DdcPowerSample{} : ddc_power_sample();
        apply_ddc_sample(topology, sample);
        if (!usable_cached()) return false;

        running_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mu_);
            next_refresh_ = std::chrono::steady_clock::now() + kHealthRefreshInterval;
        }
        worker_ = std::thread([this] { worker_loop(); });
        return true;
    }

    bool usable()
    {
        request_refresh();
        return usable_cached();
    }

    void stop()
    {
        const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
        if (was_running) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                refresh_requested_ = true;
            }
            cv_.notify_all();
        }
        if (worker_.joinable()) worker_.join();
        {
            std::lock_guard<std::mutex> lock(mu_);
            refresh_requested_ = false;
            next_refresh_ = {};
        }
        topology_.store(static_cast<int>(PhysicalSignal::Unknown), std::memory_order_release);
        ddc_.store(static_cast<int>(PhysicalSignal::Unknown), std::memory_order_release);
        ddc_single_on_confirmed_.store(false, std::memory_order_release);
        ddc_unknown_streak_.store(0, std::memory_order_release);
    }

private:
    static PhysicalSignal signal(const std::atomic<int>& value)
    {
        return static_cast<PhysicalSignal>(value.load(std::memory_order_acquire));
    }

    bool usable_cached() const
    {
        return physical_usable(signal(topology_), session_.signal(), signal(ddc_));
    }

    void apply_ddc_sample(PhysicalSignal topology, const DdcPowerSample& sample)
    {
        if (topology == PhysicalSignal::Unavailable) {
            ddc_.store(static_cast<int>(PhysicalSignal::Unknown), std::memory_order_release);
            ddc_unknown_streak_.store(0, std::memory_order_release);
            return;
        }

        PhysicalSignal effective = sample.signal;
        if (sample.confirmed_single_on()) {
            ddc_single_on_confirmed_.store(true, std::memory_order_release);
            ddc_unknown_streak_.store(0, std::memory_order_release);
        } else if (sample.signal == PhysicalSignal::Available) {
            // The current topology is no longer the one trusted single-monitor
            // baseline. Do not carry hard-off inference across monitor changes.
            ddc_single_on_confirmed_.store(false, std::memory_order_release);
            ddc_unknown_streak_.store(0, std::memory_order_release);
        } else if (sample.signal == PhysicalSignal::Unavailable) {
            ddc_unknown_streak_.store(0, std::memory_order_release);
        } else if (ddc_single_on_confirmed_.load(std::memory_order_acquire)) {
            const unsigned misses = ddc_unknown_streak_.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (misses >= kDdcDisappearThreshold)
                effective = PhysicalSignal::Unavailable;
        }

        ddc_.store(static_cast<int>(effective), std::memory_order_release);
    }

    void request_refresh()
    {
        if (!running_.load(std::memory_order_acquire)) return;
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (refresh_requested_ ||
                (next_refresh_.time_since_epoch().count() != 0 && now < next_refresh_))
                return;
            refresh_requested_ = true;
            next_refresh_ = now + kHealthRefreshInterval;
        }
        cv_.notify_one();
    }

    void worker_loop()
    {
        std::unique_lock<std::mutex> lock(mu_);
        while (running_.load(std::memory_order_acquire)) {
            cv_.wait(lock, [this] {
                return !running_.load(std::memory_order_acquire) || refresh_requested_;
            });
            if (!running_.load(std::memory_order_acquire)) break;
            refresh_requested_ = false;
            lock.unlock();

            const auto topology = topology_physical_signal();
            topology_.store(static_cast<int>(topology), std::memory_order_release);
            const auto sample = topology == PhysicalSignal::Unavailable ? DdcPowerSample{} : ddc_power_sample();
            apply_ddc_sample(topology, sample);

            lock.lock();
        }
    }

    SessionDisplayPower session_;
    std::atomic<int> topology_{static_cast<int>(PhysicalSignal::Unknown)};
    std::atomic<int> ddc_{static_cast<int>(PhysicalSignal::Unknown)};
    std::atomic<bool> ddc_single_on_confirmed_{false};
    std::atomic<unsigned> ddc_unknown_streak_{0};
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool refresh_requested_ = false;
    std::chrono::steady_clock::time_point next_refresh_{};
};

DisplayMode physical_mode()
{
    DisplayMode mode;
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    mode.width = width > 0 ? width : 1920;
    mode.height = height > 0 ? height : 1080;
    mode.width &= ~1;
    mode.height &= ~1;
    DEVMODEW current{};
    current.dmSize = sizeof(current);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &current) && current.dmDisplayFrequency > 1)
        mode.refresh_hz = static_cast<int>(current.dmDisplayFrequency);
    return mode;
}

HANDLE open_driver()
{
    return CreateFileW(idd::kDevicePath, GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

bool send_ioctl(HANDLE driver, DWORD code, const void* input, DWORD input_size,
                void* output = nullptr, DWORD output_size = 0)
{
    if (!driver || driver == INVALID_HANDLE_VALUE) return false;
    DWORD returned = 0;
    return DeviceIoControl(driver, code, const_cast<void*>(input), input_size,
                           output, output_size, &returned, nullptr) != FALSE;
}

bool query_status(HANDLE driver, idd::DriverStatus& status)
{
    status = {};
    if (!send_ioctl(driver, idd::kIoctlGetStatus, nullptr, 0,
                    &status, static_cast<DWORD>(sizeof(status))))
        return false;
    return status.version == idd::kProtocolVersion;
}

DisplayMode status_mode(const idd::DriverStatus& status)
{
    DisplayMode mode;
    mode.width = status.width ? static_cast<int>(status.width) : 1920;
    mode.height = status.height ? static_cast<int>(status.height) : 1080;
    mode.refresh_hz = status.refresh_hz ? static_cast<int>(status.refresh_hz) : 60;
    mode.scale = 1.0f;
    return mode;
}

class WindowsDisplayBackend final : public DisplayBackend {
public:
    ~WindowsDisplayBackend() override = default;

    bool probe(DisplayTarget& target) override
    {
        error_ = {};
        physical_health_.stop();
        if (physical_health_.prepare()) {
            HANDLE driver = open_driver();
            if (driver != INVALID_HANDLE_VALUE) {
                idd::DriverStatus status{};
                if (query_status(driver, status) && status.monitor_active)
                    (void)send_ioctl(driver, idd::kIoctlDestroyMonitor, nullptr, 0);
                CloseHandle(driver);
            }
            target = {};
            target.kind = DisplayKind::Physical;
            target.capture_kind = DisplayCaptureKind::Desktop;
            target.mode = physical_mode();
            target.name = "Windows desktop";
            backend_ = "win32-physical";
            return true;
        }

        HANDLE driver = open_driver();
        if (driver == INVALID_HANDLE_VALUE) return false;
        idd::DriverStatus status{};
        const bool ok = query_status(driver, status) && status.monitor_active;
        CloseHandle(driver);
        if (!ok) return false;

        target = {};
        target.kind = DisplayKind::VirtualExistingSession;
        target.capture_kind = DisplayCaptureKind::WindowsIddSwapchain;
        target.mode = status_mode(status);
        target.name = "OPAL Virtual Display";
        target.native_id = 1;
        target.owned_by_opal = true;
        backend_ = "iddcx-virtual";
        return true;
    }

    bool ensure(const DisplayMode& requested, DisplayTarget& target) override
    {
        error_ = {};
        physical_health_.stop();
        HANDLE driver = open_driver();
        if (driver == INVALID_HANDLE_VALUE) {
            return fail(PlatformFailure::DependencyMissing,
                        "OPAL virtual display driver is not installed or not running");
        }

        idd::DriverStatus status{};
        if (!query_status(driver, status) || !status.adapter_ready) {
            CloseHandle(driver);
            return fail(PlatformFailure::Unavailable,
                        "OPAL virtual display driver adapter is not ready");
        }

        idd::DisplayModeRequest request;
        request.width = static_cast<std::uint32_t>(
            std::clamp(requested.width, 640, static_cast<int>(idd::kMaxWidth)) & ~1);
        request.height = static_cast<std::uint32_t>(
            std::clamp(requested.height, 480, static_cast<int>(idd::kMaxHeight)) & ~1);
        request.refresh_hz = static_cast<std::uint32_t>(std::clamp(requested.refresh_hz, 30, 240));

        const DWORD command = status.monitor_active ? idd::kIoctlSetMode : idd::kIoctlCreateMonitor;
        if (!send_ioctl(driver, command, &request, static_cast<DWORD>(sizeof(request)))) {
            const DWORD win_error = GetLastError();
            CloseHandle(driver);
            return fail(PlatformFailure::OsError,
                        "OPAL virtual display driver rejected monitor creation (Win32 " +
                        std::to_string(win_error) + ")");
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        do {
            if (query_status(driver, status) && status.monitor_active) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while (std::chrono::steady_clock::now() < deadline);
        CloseHandle(driver);

        if (!status.monitor_active)
            return fail(PlatformFailure::Unavailable, "OPAL virtual monitor did not become active");

        target = {};
        target.kind = DisplayKind::VirtualExistingSession;
        target.capture_kind = DisplayCaptureKind::WindowsIddSwapchain;
        target.mode = status_mode(status);
        target.name = "OPAL Virtual Display";
        target.native_id = 1;
        target.owned_by_opal = true;
        backend_ = "iddcx-virtual";
        return true;
    }

    bool reconfigure(const DisplayMode& mode, DisplayTarget& target) override
    {
        if (!target.virtual_display()) return true;
        return ensure(mode, target);
    }

    bool healthy(const DisplayTarget& target) override
    {
        if (!target.virtual_display()) return physical_health_.usable();
        HANDLE driver = open_driver();
        if (driver == INVALID_HANDLE_VALUE) return false;
        idd::DriverStatus status{};
        const bool ok = query_status(driver, status) && status.monitor_active;
        CloseHandle(driver);
        return ok;
    }

    void release(DisplayTarget& target) override
    {
        physical_health_.stop();
        if (target.owned_by_opal) {
            HANDLE driver = open_driver();
            if (driver != INVALID_HANDLE_VALUE) {
                (void)send_ioctl(driver, idd::kIoctlDestroyMonitor, nullptr, 0);
                CloseHandle(driver);
            }
        }
        target = {};
        backend_ = "win32-display";
    }

    std::string backend_name() const override { return backend_; }
    PlatformError last_platform_error() const override { return error_; }

private:
    bool fail(PlatformFailure failure, std::string message)
    {
        error_ = {PlatformComponent::Capture, failure, std::move(message), false};
        return false;
    }

    PhysicalHealthMonitor physical_health_;
    std::string backend_ = "win32-display";
    PlatformError error_{};
};

}

std::unique_ptr<DisplayBackend> make_display_backend()
{
    return std::make_unique<WindowsDisplayBackend>();
}

}
