#pragma once
#include <cstddef>
#include <string>
#include <sys/types.h>

namespace opal {
struct CaptureProcess {
    pid_t pid=-1;
    int fd=-1;
};
std::string capture_command(bool gpu_screen_recorder,int fps,int bitrate_kbps,bool audio);
CaptureProcess start_capture(const std::string &command);
int read_capture(CaptureProcess &capture,void *buffer,size_t size,int timeout_ms);
void stop_capture(CaptureProcess &capture);
}
