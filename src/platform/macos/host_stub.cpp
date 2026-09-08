#include <opal/host.hpp>

#include <iostream>

namespace opal {
namespace {
int unavailable(const char *operation)
{
    std::cerr << "OPAL macOS " << operation
              << " is not available until the native ScreenCaptureKit/VideoToolbox host backend is enabled.\n";
    return 1;
}
}

int host_setup(){return unavailable("host setup");}
int host_run(){return unavailable("host");}
int host_daemon(){return unavailable("host daemon");}

}
