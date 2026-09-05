#pragma once
#include <opal/media_profile.hpp>
#include <chrono>
#include <cstddef>
#include <string>
#include <sys/types.h>

namespace opal {
struct CaptureProcess { pid_t pid=-1; int fd=-1; };
struct SinkProcess { pid_t pid=-1; int fd=-1; bool compact_input=false; };
std::string capture_command(bool gpu_screen_recorder,int fps,int bitrate_kbps,bool audio,const std::string &portal_token_file="",int max_width=0,int max_height=0);
CaptureProcess start_capture(const std::string &command);
int read_capture(CaptureProcess &capture,void *buffer,size_t size,int timeout_ms);
void stop_capture(CaptureProcess &capture);
SinkProcess start_sink(const std::string &command);
bool write_sink_timeout(SinkProcess &sink,const void *data,size_t size,int timeout_ms);
void stop_sink(SinkProcess &sink);
}
