#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <SDL3/SDL.h>
#include <sys/stat.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace opal {
namespace {

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
    return windows_host_run_impl();
}

int host_daemon()
{
    ensure_windows_input_helper_environment();
    return windows_host_daemon_impl();
}

}
