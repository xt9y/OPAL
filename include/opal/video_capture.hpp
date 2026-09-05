#pragma once
#include <opal/media_profile.hpp>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace opal {
enum class MediaKind { VideoH264, AudioAac };
enum class CaptureTimestampQuality : std::uint8_t { Estimated=0, Exact=1 };
constexpr std::string_view capture_timestamp_quality_name(CaptureTimestampQuality quality){return quality==CaptureTimestampQuality::Exact?"exact":"estimated";}
struct MediaConfig {MediaKind kind=MediaKind::VideoH264;std::vector<std::uint8_t> extradata;int sample_rate=0;int channels=0;};
struct EncodedMediaView {MediaKind kind=MediaKind::VideoH264;std::span<const std::uint8_t> data;std::int64_t pts_us=0;std::uint64_t capture_time_us=0;bool keyframe=false;};
struct EncodedMediaUnit {MediaKind kind=MediaKind::VideoH264;std::vector<std::uint8_t> data;std::int64_t pts_us=0;std::uint64_t capture_time_us=0;bool keyframe=false;};
class VideoCapture {
public:
    VideoCapture();~VideoCapture();VideoCapture(const VideoCapture&)=delete;VideoCapture& operator=(const VideoCapture&)=delete;
    bool start(const StreamOptions &stream,int bitrate_kbps,bool audio,const std::string &portal_token_file);
    bool next_view(EncodedMediaView &unit,int timeout_ms);
    bool next(EncodedMediaUnit &unit,int timeout_ms);
    bool ended() const;
    const std::vector<MediaConfig>& configs() const;
    std::uint64_t config_revision() const;
    std::string backend_name() const;
    std::string last_error() const;
    CaptureTimestampQuality capture_timestamp_quality() const;
    bool capture_timestamp_estimated() const;
    void stop();
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}