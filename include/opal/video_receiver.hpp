#pragma once

#include <opal/direct_video_session.hpp>
#include <X11/Xlib.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace opal {

class VideoReceiver {
public:
    VideoReceiver();
    VideoReceiver(const VideoReceiver&)=delete;
    VideoReceiver& operator=(const VideoReceiver&)=delete;
    bool start(DirectVideoPath path,std::function<void(const std::string&)> control_send);
    bool media_started() const;
    Window presentation_window() const;
    std::uint64_t stale_frames() const;
    std::uint64_t highest_sequence() const;
    void stop();
    ~VideoReceiver();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
