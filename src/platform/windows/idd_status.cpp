#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>

#include <opal/windows_idd.hpp>

#include "../../../platform/windows/idd/Protocol.hpp"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

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

BOOL CALLBACK collect_physical_hmonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data)
{
    auto* monitors = reinterpret_cast<std::vector<HMONITOR>*>(data);
    if (!monitors) return FALSE;
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) && !gdi_device_is_opal(info.szDevice))
        monitors->push_back(monitor);
    return TRUE;
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
        if (opal_display_text(device.DeviceID) || opal_display_text(device.DeviceString))
            continue;
        return true;
    }
    return false;
}

bool windows_physical_display_usable()
{
    if (!windows_physical_display_present()) return false;

    HMODULE module = LoadLibraryW(L"Dxva2.dll");
    if (!module) return true;

    using GetCountFn = BOOL (WINAPI*)(HMONITOR, LPDWORD);
    using GetMonitorsFn = BOOL (WINAPI*)(HMONITOR, DWORD, LPPHYSICAL_MONITOR);
    using DestroyMonitorsFn = BOOL (WINAPI*)(DWORD, LPPHYSICAL_MONITOR);
    using GetVcpFn = BOOL (WINAPI*)(HANDLE, BYTE, LPMC_VCP_CODE_TYPE, LPDWORD, LPDWORD);

    const auto get_count = reinterpret_cast<GetCountFn>(
        GetProcAddress(module, "GetNumberOfPhysicalMonitorsFromHMONITOR"));
    const auto get_monitors = reinterpret_cast<GetMonitorsFn>(
        GetProcAddress(module, "GetPhysicalMonitorsFromHMONITOR"));
    const auto destroy_monitors = reinterpret_cast<DestroyMonitorsFn>(
        GetProcAddress(module, "DestroyPhysicalMonitors"));
    const auto get_vcp = reinterpret_cast<GetVcpFn>(
        GetProcAddress(module, "GetVCPFeatureAndVCPFeatureReply"));

    if (!get_count || !get_monitors || !destroy_monitors || !get_vcp) {
        FreeLibrary(module);
        return true;
    }

    std::vector<HMONITOR> monitors;
    if (!EnumDisplayMonitors(nullptr, nullptr, collect_physical_hmonitor,
                             reinterpret_cast<LPARAM>(&monitors)) || monitors.empty()) {
        FreeLibrary(module);
        return true;
    }

    unsigned known_off = 0;
    unsigned unknown = 0;
    bool known_on = false;

    for (HMONITOR monitor : monitors) {
        DWORD count = 0;
        if (!get_count(monitor, &count) || count == 0) {
            ++unknown;
            continue;
        }

        std::vector<PHYSICAL_MONITOR> physical(count);
        if (!get_monitors(monitor, count, physical.data())) {
            unknown += count;
            continue;
        }

        for (const auto& item : physical) {
            DWORD current = 0;
            DWORD maximum = 0;
            MC_VCP_CODE_TYPE type{};
            if (!get_vcp(item.hPhysicalMonitor, 0xd6, &type, &current, &maximum)) {
                ++unknown;
                continue;
            }
            if (current == 0x01)
                known_on = true;
            else if (current >= 0x02 && current <= 0x05)
                ++known_off;
            else
                ++unknown;
        }
        (void)destroy_monitors(count, physical.data());
    }

    FreeLibrary(module);
    if (known_on) return true;
    if (known_off > 0 && unknown == 0) return false;
    return true;
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
