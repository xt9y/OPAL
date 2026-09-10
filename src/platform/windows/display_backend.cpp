#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <opal/display_backend.hpp>

#include "../../../platform/windows/idd/Protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <memory>
#include <string>
#include <thread>

namespace opal {
namespace {

bool contains_case_insensitive(const wchar_t* value, const wchar_t* needle)
{
    if (!value || !needle || !*needle) return false;
    std::wstring haystack(value);
    std::wstring pattern(needle);
    std::transform(haystack.begin(), haystack.end(), haystack.begin(), ::towupper);
    std::transform(pattern.begin(), pattern.end(), pattern.begin(), ::towupper);
    return haystack.find(pattern) != std::wstring::npos;
}

bool physical_display_present()
{
    for (DWORD index = 0;; ++index) {
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        if (!EnumDisplayDevicesW(nullptr, index, &device, 0)) break;
        if ((device.StateFlags & DISPLAY_DEVICE_ACTIVE) == 0 ||
            (device.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) != 0)
            continue;
        if (contains_case_insensitive(device.DeviceID, L"OPALDISPLAY") ||
            contains_case_insensitive(device.DeviceString, L"OPAL Virtual Display"))
            continue;
        return true;
    }
    return false;
}

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
        if (physical_display_present()) {
            // Clean up a virtual monitor left by a crashed previous host once a
            // real desktop is available again.
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
        request.width = static_cast<std::uint32_t>(std::clamp(requested.width, 640, static_cast<int>(idd::kMaxWidth)) & ~1);
        request.height = static_cast<std::uint32_t>(std::clamp(requested.height, 480, static_cast<int>(idd::kMaxHeight)) & ~1);
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
        if (!target.virtual_display()) return physical_display_present();
        HANDLE driver = open_driver();
        if (driver == INVALID_HANDLE_VALUE) return false;
        idd::DriverStatus status{};
        const bool ok = query_status(driver, status) && status.monitor_active;
        CloseHandle(driver);
        return ok;
    }

    void release(DisplayTarget& target) override
    {
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

    std::string backend_ = "win32-display";
    PlatformError error_{};
};

}

std::unique_ptr<DisplayBackend> make_display_backend()
{
    return std::make_unique<WindowsDisplayBackend>();
}

}
