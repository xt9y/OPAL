#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <windows.h>
#include <newdev.h>
#include <setupapi.h>
#include <initguid.h>
#include <devguid.h>

#include <algorithm>
#include <cwchar>
#include <iostream>
#include <string>
#include <vector>

namespace {
constexpr wchar_t kHardwareId[] = L"Root\\OPALDISPLAY";
constexpr wchar_t kInfEnvironment[] = L"OPAL_IDD_INF";

bool hardware_ids_contain(const std::vector<wchar_t>& values, const wchar_t* wanted)
{
    if (values.empty()) return false;
    const wchar_t* item = values.data();
    const wchar_t* end = values.data() + values.size();
    while (item < end && *item) {
        if (_wcsicmp(item, wanted) == 0) return true;
        item += std::wcslen(item) + 1;
    }
    return false;
}

bool device_has_hardware_id(HDEVINFO set, SP_DEVINFO_DATA& device)
{
    DWORD type = 0;
    DWORD bytes = 0;
    (void)SetupDiGetDeviceRegistryPropertyW(set, &device, SPDRP_HARDWAREID,
                                             &type, nullptr, 0, &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(wchar_t)) return false;
    std::vector<wchar_t> ids(bytes / sizeof(wchar_t) + 2, L'\0');
    if (!SetupDiGetDeviceRegistryPropertyW(set, &device, SPDRP_HARDWAREID,
                                            &type, reinterpret_cast<PBYTE>(ids.data()),
                                            static_cast<DWORD>(ids.size() * sizeof(wchar_t)), &bytes))
        return false;
    return hardware_ids_contain(ids, kHardwareId);
}

bool root_device_exists()
{
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA device{};
        device.cbSize = sizeof(device);
        if (!SetupDiEnumDeviceInfo(set, index, &device)) break;
        if (device_has_hardware_id(set, device)) { found = true; break; }
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

bool create_root_device()
{
    HDEVINFO set = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_DISPLAY, nullptr);
    if (set == INVALID_HANDLE_VALUE) return false;

    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    bool ok = SetupDiCreateDeviceInfoW(set, L"OPAL Virtual Display", &GUID_DEVCLASS_DISPLAY,
                                        nullptr, nullptr, DICD_GENERATE_ID, &device) != FALSE;
    const wchar_t hardware_ids[] = L"Root\\OPALDISPLAY\0\0";
    if (ok) {
        ok = SetupDiSetDeviceRegistryPropertyW(
                 set, &device, SPDRP_HARDWAREID,
                 reinterpret_cast<const BYTE*>(hardware_ids),
                 static_cast<DWORD>(sizeof(hardware_ids))) != FALSE;
    }
    if (ok) ok = SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &device) != FALSE;
    SetupDiDestroyDeviceInfoList(set);
    return ok;
}

bool remove_root_devices()
{
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    bool found = false;
    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA device{};
        device.cbSize = sizeof(device);
        if (!SetupDiEnumDeviceInfo(set, index, &device)) break;
        if (!device_has_hardware_id(set, device)) continue;
        found = true;
        SP_REMOVEDEVICE_PARAMS remove{};
        remove.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        remove.ClassInstallHeader.InstallFunction = DIF_REMOVE;
        remove.Scope = DI_REMOVEDEVICE_GLOBAL;
        remove.HwProfile = 0;
        if (!SetupDiSetClassInstallParamsW(set, &device,
                                            &remove.ClassInstallHeader, sizeof(remove)) ||
            !SetupDiCallClassInstaller(DIF_REMOVE, set, &device))
            ok = false;
    }
    SetupDiDestroyDeviceInfoList(set);
    return ok || !found;
}

std::wstring absolute_path(const wchar_t* value)
{
    if (!value || !*value) return {};
    DWORD needed = GetFullPathNameW(value, 0, nullptr, nullptr);
    if (!needed) return {};
    std::wstring result(needed, L'\0');
    DWORD written = GetFullPathNameW(value, needed, result.data(), nullptr);
    if (!written || written >= needed) return {};
    result.resize(written);
    return result;
}

std::wstring environment_value(const wchar_t* name)
{
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (!needed) return {};
    std::wstring value(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
    if (!written || written >= needed) return {};
    value.resize(written);
    return value;
}

int install_driver(const wchar_t* inf)
{
    const auto path = absolute_path(inf);
    if (path.empty()) {
        std::wcerr << L"Could not resolve driver INF path.\n";
        return 1;
    }

    const bool created = root_device_exists() || create_root_device();
    if (!created) {
        std::cerr << "Could not create ROOT\\OPALDISPLAY device. Win32=" << GetLastError() << "\n";
        return 1;
    }

    BOOL reboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, kHardwareId, path.c_str(),
                                             INSTALLFLAG_FORCE, &reboot)) {
        const DWORD error = GetLastError();
        (void)remove_root_devices();
        std::cerr << "Could not install OPAL display driver. Win32=" << error << "\n";
        return 1;
    }

    std::cout << "OPAL virtual display driver installed.";
    if (reboot) std::cout << " A Windows restart is required.";
    std::cout << "\n";
    return reboot ? 10 : 0;
}

int uninstall_driver()
{
    if (!remove_root_devices()) {
        std::cerr << "Could not remove OPAL virtual display device. Win32=" << GetLastError() << "\n";
        return 1;
    }
    std::cout << "OPAL virtual display device removed.\n";
    return 0;
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc >= 2 && _wcsicmp(argv[1], L"uninstall") == 0) return uninstall_driver();
    if (argc >= 2 && _wcsicmp(argv[1], L"install") == 0) {
        std::wstring inf;
        if (argc >= 3) inf = argv[2];
        else inf = environment_value(kInfEnvironment);
        if (inf.empty()) {
            std::wcerr << L"No OPAL display-driver INF path was provided.\n";
            return 2;
        }
        return install_driver(inf.c_str());
    }
    std::wcerr << L"usage: opal-display-install install [OpalDisplay.inf] | uninstall\n";
    return 2;
}
