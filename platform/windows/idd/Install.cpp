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

std::wstring parent_path(const std::wstring& path)
{
    const auto pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring{} : path.substr(0, pos);
}

std::wstring join_path(const std::wstring& directory, const wchar_t* name)
{
    if (directory.empty()) return name ? std::wstring(name) : std::wstring{};
    if (!name || !*name) return directory;
    const wchar_t last = directory.back();
    return directory + ((last == L'\\' || last == L'/') ? L"" : L"\\") + name;
}

bool file_exists(const std::wstring& path)
{
    if (path.empty()) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool complete_driver_package(const std::wstring& inf)
{
    const auto directory = parent_path(inf);
    return file_exists(inf) &&
           file_exists(join_path(directory, L"opaldisplay.cat")) &&
           file_exists(join_path(directory, L"OPALDisplay.dll"));
}

std::wstring resolve_package_inf(const wchar_t* value)
{
    const auto input = absolute_path(value);
    if (input.empty()) return {};
    if (complete_driver_package(input)) return input;

    const auto packaged = join_path(join_path(parent_path(input), L"OpalDisplay"), L"OpalDisplay.inf");
    if (complete_driver_package(packaged)) return packaged;
    return {};
}

std::wstring find_test_certificate(const std::wstring& package_inf)
{
    const auto package_directory = parent_path(package_inf);
    const std::wstring candidates[] = {
        join_path(package_directory, L"OPALDisplay.cer"),
        join_path(parent_path(package_directory), L"OPALDisplay.cer")
    };
    for (const auto& candidate : candidates)
        if (file_exists(candidate)) return candidate;
    return {};
}

std::wstring quote_argument(const std::wstring& value)
{
    if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t c : value) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'\"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(c);
            slashes = 0;
            continue;
        }
        result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(c);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

std::wstring system_executable(const wchar_t* name)
{
    wchar_t directory[MAX_PATH]{};
    const UINT written = GetSystemDirectoryW(directory, MAX_PATH);
    if (!written || written >= MAX_PATH) return {};
    return join_path(directory, name);
}

bool run_process(const std::wstring& executable, const std::vector<std::wstring>& arguments)
{
    if (executable.empty()) return false;
    std::wstring command = quote_argument(executable);
    for (const auto& argument : arguments) {
        command.push_back(L' ');
        command += quote_argument(argument);
    }
    std::vector<wchar_t> command_line(command.begin(), command.end());
    command_line.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        return false;

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    const bool read_exit = GetExitCodeProcess(process.hProcess, &exit_code) != FALSE;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return read_exit && exit_code == 0;
}

bool trust_test_certificate(const std::wstring& certificate)
{
    if (certificate.empty()) return true;
    const auto certutil = system_executable(L"certutil.exe");
    if (certutil.empty()) return false;
    return run_process(certutil, {L"-addstore", L"-f", L"Root", certificate}) &&
           run_process(certutil, {L"-addstore", L"-f", L"TrustedPublisher", certificate});
}

std::string win32_message(DWORD error)
{
    char* buffer = nullptr;
    const DWORD size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                          FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, error, 0, reinterpret_cast<char*>(&buffer), 0, nullptr);
    std::string result;
    if (size && buffer) {
        result.assign(buffer, size);
        while (!result.empty() && (result.back() == '\r' || result.back() == '\n' || result.back() == ' '))
            result.pop_back();
    }
    if (buffer) LocalFree(buffer);
    return result;
}

int install_driver(const wchar_t* inf)
{
    const auto path = resolve_package_inf(inf);
    if (path.empty()) {
        std::wcerr << L"Could not locate a complete OPAL driver package (.inf + .cat + .dll).\n";
        return 1;
    }

    const auto certificate = find_test_certificate(path);
    if (!certificate.empty() && !trust_test_certificate(certificate)) {
        std::wcerr << L"Could not trust the WDK test certificate: " << certificate << L"\n";
        return 1;
    }

    const bool created = root_device_exists() || create_root_device();
    if (!created) {
        const DWORD error = GetLastError();
        std::cerr << "Could not create ROOT\\OPALDISPLAY device. Win32=" << error;
        const auto message = win32_message(error);
        if (!message.empty()) std::cerr << " (" << message << ")";
        std::cerr << "\n";
        return 1;
    }

    BOOL reboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, kHardwareId, path.c_str(),
                                             INSTALLFLAG_FORCE, &reboot)) {
        const DWORD error = GetLastError();
        (void)remove_root_devices();
        std::cerr << "Could not install OPAL display driver. Win32=" << error;
        const auto message = win32_message(error);
        if (!message.empty()) std::cerr << " (" << message << ")";
        std::cerr << "\nSee C:\\Windows\\INF\\setupapi.dev.log for the exact PnP failure.\n";
        std::cerr << "If Windows rejects the WDK test signature, enable test signing from an elevated "
                     "terminal with: bcdedit /set testsigning on\n";
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
        const DWORD error = GetLastError();
        std::cerr << "Could not remove OPAL virtual display device. Win32=" << error;
        const auto message = win32_message(error);
        if (!message.empty()) std::cerr << " (" << message << ")";
        std::cerr << "\n";
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
