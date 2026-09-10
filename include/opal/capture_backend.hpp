#pragma once

#include <opal/display_backend.hpp>
#include <opal/media_profile.hpp>
#include <opal/native_video_frame.hpp>
#include <opal/platform_error.hpp>
#include <opal/video_capture.hpp>

#include <memory>
#include <string>

namespace opal {

class CaptureBackend {
public:
    virtual ~CaptureBackend() = default;
    virtual bool start(const StreamOptions& stream) = 0;
    virtual bool start(const StreamOptions& stream, const DisplayTarget* target)
    {
        (void)target;
        return start(stream);
    }
    virtual bool next(NativeVideoFrame& frame, int timeout_ms) = 0;
    virtual void stop() = 0;
    virtual CaptureTimestampQuality timestamp_quality() const = 0;
    virtual std::string backend_name() const = 0;
    virtual PlatformError last_platform_error() const = 0;
};

std::unique_ptr<CaptureBackend> make_capture_backend();

}
