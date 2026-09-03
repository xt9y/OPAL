#pragma once

#include <cstdint>
#include <span>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

namespace opal {

struct DecodedVideoFrame {
    AVFrame *frame=nullptr;
    std::int64_t pts_us=0;
};

class VideoDecoder {
public:
    VideoDecoder();
    VideoDecoder(const VideoDecoder&)=delete;
    VideoDecoder& operator=(const VideoDecoder&)=delete;
    bool configure_h264(std::span<const std::uint8_t> extradata);
    bool decode(std::span<const std::uint8_t> unit,std::int64_t pts_us,
                std::vector<DecodedVideoFrame>& out);
    void flush();
    ~VideoDecoder();
private:
    struct Impl;
    Impl *impl_=nullptr;
};

}
