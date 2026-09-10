#include <opal/config.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <SDL3/SDL.h>
#include <sys/stat.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace opal {
namespace {

constexpr wchar_t kHostMutexName[] = L"Local\\xt9y.OPAL.HostDaemon";
constexpr wchar_t kHostStopEventName[] = L"Local\\xt9y.OPAL.HostStop";

std::filesystem::path windows_executable_path()
{
    std::vector<wchar_t> buffer(1024);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size() - 1) return std::filesystem::path(std::wstring(buffer.data(), length));
        if (buffer.size() >= 32768) return {};
        buffer.resize(buffer.size() * 2);
    }
}

bool windows_debug_enabled()
{
    const char* value = std::getenv("OPAL_DEBUG");
    return value && *value && std::string(value) != "0";
}

class WindowsHostPowerGuard {
public:
    WindowsHostPowerGuard()
    {
        previous_ = SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
        if (previous_ == 0 && windows_debug_enabled())
            std::cerr << "OPAL Windows power hold failed error=" << GetLastError() << '\n';
    }

    ~WindowsHostPowerGuard()
    {
        (void)SetThreadExecutionState(ES_CONTINUOUS);
    }

    WindowsHostPowerGuard(const WindowsHostPowerGuard&) = delete;
    WindowsHostPowerGuard& operator=(const WindowsHostPowerGuard&) = delete;

private:
    EXECUTION_STATE previous_ = 0;
};

void ensure_windows_input_helper_environment()
{
    if (const char* configured = std::getenv("OPAL_INPUT_HELPER"); configured && *configured) return;
    const auto executable = windows_executable_path();
    if (executable.empty()) return;

    std::error_code error;
    const auto adjacent = executable.parent_path() / L"opal-input.exe";
    if (std::filesystem::is_regular_file(adjacent, error) && !error) {
        const auto value = adjacent.string();
        (void)_putenv_s("OPAL_INPUT_HELPER", value.c_str());
        return;
    }

    error.clear();
    const auto dev = executable.parent_path() / L"build" / L"opal-input.exe";
    if (std::filesystem::is_regular_file(dev, error) && !error) {
        const auto value = dev.string();
        (void)_putenv_s("OPAL_INPUT_HELPER", value.c_str());
    }
}

const SDL_DisplayMode* windows_virtual_desktop_mode(SDL_DisplayID display)
{
    const SDL_DisplayMode* native = SDL_GetDesktopDisplayMode(display);
    static thread_local SDL_DisplayMode mode{};
    if (native) mode = *native;
    else mode = {};

    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width > 0) mode.w = width;
    if (height > 0) mode.h = height;
    return mode.w > 0 && mode.h > 0 ? &mode : native;
}

int windows_ignore_posix_mode(const wchar_t*, int)
{
    // Files live below the interactive user's profile and inherit its Windows
    // ACL. POSIX chmod mode bits have no useful equivalent here.
    return 0;
}

std::filesystem::path windows_host_pid_path()
{
    return Paths::load().root / "host.pid";
}

bool write_windows_host_pid()
{
    const auto paths = Paths::load();
    if (!ensure_layout(paths)) return false;
    std::ofstream output(paths.root / "host.pid", std::ios::out | std::ios::trunc);
    if (!output) return false;
    output << GetCurrentProcessId() << '\n';
    return output.good();
}

void clear_windows_host_pid(DWORD expected_pid)
{
    const auto path = windows_host_pid_path();
    std::ifstream input(path);
    unsigned long long pid = 0;
    if (!(input >> pid) || pid != expected_pid) return;
    input.close();
    std::error_code error;
    std::filesystem::remove(path, error);
}

}
}

#define host_setup windows_host_setup_impl
#define host_run windows_host_run_impl
#define host_daemon windows_host_daemon_impl
#define SDL_GetDesktopDisplayMode windows_virtual_desktop_mode
#define chmod(path, mode) windows_ignore_posix_mode(path, mode)
#include "../../host.cpp"
#undef chmod
#undef SDL_GetDesktopDisplayMode
#undef host_setup
#undef host_run
#undef host_daemon

namespace opal {

int host_setup()
{
    ensure_windows_input_helper_environment();
    return windows_host_setup_impl();
}

int host_run()
{
    ensure_windows_input_helper_environment();
    WindowsHostPowerGuard power_guard;
    return windows_host_run_impl();
}

int host_daemon()
{
    ensure_windows_input_helper_environment();

    HANDLE single_instance = CreateMutexW(nullptr, TRUE, kHostMutexName);
    if (!single_instance) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(single_instance);
        return 0;
    }

    HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, kHostStopEventName);
    if (!stop_event) {
        ReleaseMutex(single_instance);
        CloseHandle(single_instance);
        return 1;
    }
    (void)ResetEvent(stop_event);

    const DWORD pid = GetCurrentProcessId();
    if (!write_windows_host_pid()) {
        CloseHandle(stop_event);
        ReleaseMutex(single_instance);
        CloseHandle(single_instance);
        return 1;
    }

    std::thread([stop_event] {
        if (WaitForSingleObject(stop_event, INFINITE) == WAIT_OBJECT_0)
            ExitProcess(0);
    }).detach();

    WindowsHostPowerGuard power_guard;
    const int result = windows_host_daemon_impl();
    clear_windows_host_pid(pid);
    ReleaseMutex(single_instance);
    CloseHandle(single_instance);
    return result;
}

}
