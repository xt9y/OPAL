#pragma once
#include <cstdint>
#include <string>

namespace opal {
struct StreamOptions {
    int max_width=1920;
    int max_height=1080;
    int fps=60;
};

StreamOptions default_stream_options();
bool stream_mode_limit(const std::string &mode,int &max_width,int &max_height);
int automatic_bitrate_kbps(int width,int height,int fps);
std::uint64_t capture_stale_budget_us(int fps);
int normal_gop_frames(int fps);
std::uint64_t sender_burst_budget_bytes(int bitrate_kbps,int fps,bool keyframe);
int sender_pacing_rate_kbps(int bitrate_kbps,bool keyframe);
int encoder_reconfigure_bitrate_kbps(int active_kbps,int target_kbps,std::uint64_t since_restart_ms);
}
