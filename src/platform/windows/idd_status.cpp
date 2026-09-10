#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <opal/windows_idd.hpp>

#include "../../../platform/windows/idd/Protocol.hpp"

#include <algorithm>
#include <cwctype>
#include <string>

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

}

bool windows_physical_display_present()
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

WindowsIddStatus windows_idd_status()
{
    WindowsIddStatus result;
    HANDLE driver = CreateFileW(idd::kDevicePath, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (driver == INVALID_HANDLE_VALUE) return result;
    result.device_present = true;

    idd::DriverStatus status{};
    DWORD returned = 0;
    if (DeviceIoControl(driver, idd::kIoctlGetStatus, nullptr, 0,
                        &status, static_cast<DWORD>(sizeof(status)), &returned, nullptr) &&
        returned >= sizeof(status) && status.version == idd::kProtocolVersion) {
        result.adapter_ready = status.adapter_ready != 0;
        result.monitor_active = status.monitor_active != 0;
        result.width = static_cast<int>(status.width);
        result.height = static_cast<int>(status.height);
        result.refresh_hz = static_cast<int>(status.refresh_hz);
    }
    CloseHandle(driver);
    return result;
}

}
