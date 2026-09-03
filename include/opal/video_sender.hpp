#pragma once

#include <opal/direct_video_session.hpp>
#include <opal/media_profile.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace opal {

class VideoSender {
public:
    VideoSender();
    VideoSender(const VideoSender&)=delete;
    VideoSender& operator=(const VideoSender&)=delete;
    bool start(DirectVideoPath path,const StreamOptions& stream,bool audio,
               std::function<void(const std::string&)> control_send);
    void request_idr();
    bool handle_control_line(const std::string& line);
    void set_target_bitrate(int kbps);
    int target_bitrate() const;
    std::size_t queued_frames() const;
    std::size_t queued_bytes() const;
    std::uint64_t stale_frames() const;
    void stop();
    ~VideoSender();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
