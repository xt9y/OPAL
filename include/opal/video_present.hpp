#pragma once

#include <opal/video_decoder.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace opal {

class VideoPresenter {
public:
    VideoPresenter();
    VideoPresenter(const VideoPresenter&)=delete;
    VideoPresenter& operator=(const VideoPresenter&)=delete;
    bool open(int source_width,int source_height,bool fullscreen=true);
    bool present_borrowed(DecodedVideoView frame);
    bool present(DecodedVideoFrame frame);
    std::pair<int,int> drawable_size() const;
    std::pair<int,int> window_size() const;
    bool set_relative_mouse_mode(bool enabled);
    std::size_t pending_frame_count() const;
    std::uint64_t presented_frames() const;
    std::string backend_name() const;
    bool is_open() const;
    void close();
    ~VideoPresenter();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
