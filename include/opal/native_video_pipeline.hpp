#pragma once

#include <opal/capture_backend.hpp>
#include <opal/video_encoder_backend.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace opal {

class NativeVideoPipeline {
public:
    NativeVideoPipeline(std::unique_ptr<CaptureBackend> capture,
                        std::unique_ptr<VideoEncoderBackend> encoder);
    NativeVideoPipeline(const NativeVideoPipeline&) = delete;
    NativeVideoPipeline& operator=(const NativeVideoPipeline&) = delete;
    ~NativeVideoPipeline();

    bool start(const StreamOptions& stream, int bitrate_kbps);
    bool next(EncodedMediaUnit& unit, int timeout_ms);
    void request_idr();
    bool set_bitrate(int bitrate_kbps);
    void stop();

    const MediaConfig& config() const;
    std::uint64_t config_revision() const;
    CaptureTimestampQuality timestamp_quality() const;
    std::string backend_name() const;
    PlatformError last_platform_error() const;
    bool ended() const noexcept;

private:
    std::unique_ptr<CaptureBackend> capture_;
    std::unique_ptr<VideoEncoderBackend> encoder_;
    MediaConfig config_{};
    std::uint64_t config_revision_ = 0;
    bool running_ = false;
    bool terminal_ = false;
};

}
