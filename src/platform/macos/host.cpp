#import <ApplicationServices/ApplicationServices.h>
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

bool macos_host_permissions()
{
    bool ok = true;
    if (!CGPreflightScreenCaptureAccess()) {
        std::cerr << "OPAL macOS host requires Screen Recording permission. Enable it in System Settings > Privacy & Security > Screen & System Audio Recording.\n";
        ok = false;
    }
    if (!AXIsProcessTrusted()) {
        std::cerr << "OPAL macOS host requires Accessibility permission for keyboard and pointer injection. Enable it in System Settings > Privacy & Security > Accessibility.\n";
        ok = false;
    }
    return ok;
}

}

int host_setup()
{
    return macos_host_setup_impl();
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
