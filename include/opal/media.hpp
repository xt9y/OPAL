#pragma once
#include <chrono>
#include <cstddef>
#include <string>
#include <sys/types.h>

namespace opal {
struct CaptureProcess { pid_t pid=-1; int fd=-1; };
struct SinkProcess { pid_t pid=-1; int fd=-1; };
bool stream_mode_limit(const std::string &mode,int &max_width,int &max_height);
int automatic_bitrate_kbps(int width,int height,int fps);
std::string video_request_line(const std::string &token,int max_width,int max_height,int fps);
bool parse_video_request_line(const std::string &line,std::string &token,int &max_width,int &max_height,int &fps);
std::string capture_command(bool gpu_screen_recorder,int fps,int bitrate_kbps,bool audio,const std::string &portal_token_file="",int max_width=0,int max_height=0);
CaptureProcess start_capture(const std::string &command);
int read_capture(CaptureProcess &capture,void *buffer,size_t size,int timeout_ms);
void stop_capture(CaptureProcess &capture);
SinkProcess start_sink(const std::string &command);
bool write_sink_timeout(SinkProcess &sink,const void *data,size_t size,int timeout_ms);
void stop_sink(SinkProcess &sink);
}
