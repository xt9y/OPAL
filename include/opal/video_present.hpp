#pragma once

#include <opal/video_decoder.hpp>
#include <X11/Xlib.h>
#include <cstddef>
#include <memory>
#include <utility>

namespace opal {

class VideoPresenter {
public:
    VideoPresenter();
    VideoPresenter(const VideoPresenter&)=delete;
    VideoPresenter& operator=(const VideoPresenter&)=delete;
    bool open(int source_width,int source_height,bool fullscreen=true);
    bool present(DecodedVideoFrame frame);
    Window x11_window() const;
    std::pair<int,int> drawable_size() const;
    std::size_t pending_frame_count() const;
    void close();
    ~VideoPresenter();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
