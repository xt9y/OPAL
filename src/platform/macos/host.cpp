#import <CoreGraphics/CoreGraphics.h>

#define host_setup macos_host_setup_impl
#define host_run macos_host_run_impl
#define host_daemon macos_host_daemon_impl
#include "../../host.cpp"
#undef host_setup
#undef host_run
#undef host_daemon

namespace opal {
namespace {

void print_screen_permission_help()
{
    std::cerr << "OPAL macOS host requires Screen Recording permission. Enable it in System Settings > Privacy & Security > Screen & System Audio Recording.\n";
}

void print_accessibility_permission_help()
{
    std::cerr << "OPAL macOS host requires Accessibility permission for the opal-input helper. Enable opal-input in System Settings > Privacy & Security > Accessibility.\n";
}

bool input_helper_access(const char* mode, bool quiet)
{
    if (!mode || !*mode) return false;
    const std::string helper = input_helper_command();
    if (helper.empty()) return false;
    std::string command = helper + " " + mode;
    if (quiet) command += " >/dev/null 2>&1";
    return std::system(command.c_str()) == 0;
}

bool macos_host_permissions()
{
    bool ok = true;
    if (!CGPreflightScreenCaptureAccess()) {
        print_screen_permission_help();
        ok = false;
    }
    if (!input_helper_access("--check-access", true)) {
        print_accessibility_permission_help();
        ok = false;
    }
    return ok;
}

bool request_macos_host_permissions()
{
    bool screen_ok = CGPreflightScreenCaptureAccess();
    if (!screen_ok) screen_ok = CGRequestScreenCaptureAccess();

    const bool accessibility_ok = input_helper_access("--request-access", false);

    if (!screen_ok) print_screen_permission_help();
    if (!accessibility_ok) print_accessibility_permission_help();
    if (!screen_ok || !accessibility_ok) {
        std::cerr << "Grant the permissions above, then run 'opal' again to finish host setup.\n";
        return false;
    }
    return true;
}

}

int host_setup()
{
    if (macos_host_setup_impl() != 0) return 1;
    return request_macos_host_permissions() ? 0 : 1;
}

int host_run()
{
    if (!macos_host_permissions()) return 1;
    return macos_host_run_impl();
}

int host_daemon()
{
    if (!macos_host_permissions()) return 1;
    return macos_host_daemon_impl();
}

}
