#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

namespace opal {

struct DecodedVideoFrame {
    AVFrame *frame=nullptr;
    std::int64_t pts_us=0;
};

struct DecodedVideoView {
    const AVFrame *frame=nullptr;
    std::int64_t pts_us=0;
};

class VideoDecoder {
public:
    VideoDecoder();
    VideoDecoder(const VideoDecoder&)=delete;
    VideoDecoder& operator=(const VideoDecoder&)=delete;
    bool configure_h264(std::span<const std::uint8_t> extradata);
    bool decode_latest(std::span<const std::uint8_t> unit,std::int64_t pts_us,
                       DecodedVideoView &out,std::size_t &superseded);
    bool decode(std::span<const std::uint8_t> unit,std::int64_t pts_us,
                std::vector<DecodedVideoFrame>& out);
    std::string backend_name() const;
    void flush();
    ~VideoDecoder();
private:
    struct Impl;
    Impl *impl_=nullptr;
};

}
