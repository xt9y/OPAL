#pragma once

#include <opal/direct_video_session.hpp>
#include <X11/Xlib.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace opal {

enum class VideoReceiverFailure : std::uint8_t {
    NoFailure=0,
    PresenterOpen,
    Present,
    MediaStall
};

class VideoReceiver {
public:
    VideoReceiver();
    VideoReceiver(const VideoReceiver&)=delete;
    VideoReceiver& operator=(const VideoReceiver&)=delete;
    bool start(DirectVideoPath path,std::function<void(const std::string&)> control_send);
    bool handle_control_line(const std::string& line);
    bool media_started() const;
    bool failed() const;
    VideoReceiverFailure failure_reason() const;
    Window presentation_window() const;
    std::uint64_t stale_frames() const;
    std::uint64_t highest_sequence() const;
    std::uint32_t audio_queued_ms() const;
    void stop();
    ~VideoReceiver();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
