#pragma once
#include <opal/media_profile.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace opal {
enum class MediaKind { VideoH264, AudioAac };

struct MediaConfig {
    MediaKind kind=MediaKind::VideoH264;
    std::vector<std::uint8_t> extradata;
    int sample_rate=0;
    int channels=0;
};

struct EncodedMediaUnit {
    MediaKind kind=MediaKind::VideoH264;
    std::vector<std::uint8_t> data;
    std::int64_t pts_us=0;
    std::uint64_t capture_time_us=0;
    bool keyframe=false;
};

class VideoCapture {
public:
    VideoCapture();
    ~VideoCapture();
    VideoCapture(const VideoCapture&)=delete;
    VideoCapture& operator=(const VideoCapture&)=delete;

    bool start(const StreamOptions &stream,int bitrate_kbps,bool audio,
               const std::string &portal_token_file);
    bool next(EncodedMediaUnit &unit,int timeout_ms);
    const std::vector<MediaConfig>& configs() const;
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
