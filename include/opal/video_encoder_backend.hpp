#pragma once

#include <opal/media_profile.hpp>
#include <opal/native_video_frame.hpp>
#include <opal/platform_error.hpp>
#include <opal/video_capture.hpp>

#include <memory>
#include <string>

namespace opal {

class VideoEncoderBackend {
public:
    virtual ~VideoEncoderBackend() = default;
    virtual bool start(const StreamOptions &stream, int bitrate_kbps) = 0;
    virtual bool encode(const NativeVideoFrame &frame, EncodedMediaUnit &unit) = 0;
    virtual void request_idr() = 0;
    virtual bool set_bitrate(int bitrate_kbps) = 0;
    virtual MediaConfig config() const = 0;
    virtual std::string backend_name() const = 0;
    virtual PlatformError last_platform_error() const = 0;
    virtual void stop() = 0;
};

std::unique_ptr<VideoEncoderBackend> make_video_encoder_backend();

}
