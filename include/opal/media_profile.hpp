#pragma once
#include <cstdint>
#include <string>

namespace opal {
enum class HostDisplayMode : std::uint8_t {
    Duplicate = 0,
    Extend = 1,
};

struct StreamOptions {
    int max_width=1920;
    int max_height=1080;
    int fps=60;
    bool automatic_fps=false;
    HostDisplayMode host_display_mode=HostDisplayMode::Duplicate;

    StreamOptions() = default;

    // MEDIA_RECEIVER_READY predates host-display modes. Keep its field count
    // unchanged by carrying Extend in the otherwise-unused low bit of the
    // (always even) width. 17x16 is the Extend+native-resolution sentinel.
    StreamOptions(int wire_width,int wire_height,int frame_rate)
        : fps(frame_rate)
    {
        if (wire_width == 17 && wire_height == 16) {
            max_width = 0;
            max_height = 0;
            host_display_mode = HostDisplayMode::Extend;
            return;
        }
        host_display_mode = (wire_width > 0 && (wire_width & 1))
            ? HostDisplayMode::Extend : HostDisplayMode::Duplicate;
        max_width = wire_width > 0 ? (wire_width & ~1) : wire_width;
        max_height = wire_height;
    }

    // Preserve the old four-field aggregate-style construction used by
    // callers that specify automatic_fps explicitly.
    StreamOptions(int width,int height,int frame_rate,bool automatic)
        : max_width(width),max_height(height),fps(frame_rate),automatic_fps(automatic) {}

    StreamOptions(int width,int height,int frame_rate,bool automatic,HostDisplayMode display_mode)
        : max_width(width),max_height(height),fps(frame_rate),automatic_fps(automatic),
          host_display_mode(display_mode) {}
};

inline int stream_wire_width(const StreamOptions& stream)
{
    if (stream.host_display_mode != HostDisplayMode::Extend) return stream.max_width;
    if (stream.max_width == 0 && stream.max_height == 0) return 17;
    return stream.max_width > 0 ? (stream.max_width | 1) : stream.max_width;
}

inline int stream_wire_height(const StreamOptions& stream)
{
    if (stream.host_display_mode == HostDisplayMode::Extend &&
        stream.max_width == 0 && stream.max_height == 0)
        return 16;
    return stream.max_height;
}

StreamOptions default_stream_options();
bool stream_mode_limit(const std::string &mode,int &max_width,int &max_height);
int automatic_stream_fps(int display_refresh_hz,int capture_max_fps);
int automatic_bitrate_kbps(int width,int height,int fps);
std::uint64_t capture_stale_budget_us(int fps);
int normal_gop_frames(int fps);
int low_latency_h264_slices(int width,int height);
std::uint64_t sender_burst_budget_bytes(int bitrate_kbps,int fps,bool keyframe);
int sender_pacing_rate_kbps(int bitrate_kbps,bool keyframe);
int encoder_reconfigure_bitrate_kbps(int active_kbps,int target_kbps,std::uint64_t since_restart_ms);
}
