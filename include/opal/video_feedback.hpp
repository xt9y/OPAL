#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace opal {

struct VideoFeedbackSample {
    std::uint64_t highest_sequence=0;
    std::uint32_t received=0,lost=0,rtt_us=0,decode_age_us=0;
};

class BitrateController {
public:
    explicit BitrateController(int ceiling_kbps);
    int on_feedback(const VideoFeedbackSample&,std::chrono::steady_clock::time_point);
    int target_kbps() const;
    int floor_kbps() const;
private:
    int ceiling_=0,floor_=0,target_=0;
    std::uint32_t baseline_rtt_us_=0;
    std::chrono::steady_clock::time_point good_since_{},last_raise_{};
};

struct ClockEstimate {
    std::int64_t offset_us=0;
    std::uint32_t rtt_us=0;
    bool valid=false;
};
ClockEstimate estimate_clock_offset(std::int64_t t0_us,std::int64_t t1_us,
                                    std::int64_t t2_us,std::int64_t t3_us);

std::string video_feedback_line(std::uint32_t generation,const VideoFeedbackSample&);
bool parse_video_feedback_line(const std::string&,std::uint32_t generation,VideoFeedbackSample&);
std::string clock_sync_request_line(std::uint32_t generation,std::int64_t t0_us);
std::string clock_sync_reply_line(std::uint32_t generation,std::int64_t t0_us,
                                  std::int64_t t1_us,std::int64_t t2_us);
bool parse_clock_sync_line(const std::string&,std::uint32_t generation,
                           std::int64_t &t0_us,std::int64_t &t1_us,std::int64_t &t2_us);

struct LatencyTelemetry {
    double capture_to_packet_ms=0,network_ms=0,reassembly_ms=0,
           decode_ms=0,present_ms=0,total_ms=0,loss_percent=0;
    std::uint64_t stale_frames=0;
    int bitrate_kbps=0;
};
std::string format_latency_telemetry(const LatencyTelemetry&);

}
