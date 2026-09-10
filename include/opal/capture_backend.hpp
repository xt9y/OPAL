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
#if defined(_WIN32)
        // A generic Windows Desktop Duplication backend cannot select an OPAL
        // IddCx target yet. Reject that target so WindowsIddCaptureBackend
        // falls through to the driver's dedicated frame channel instead of
        // accidentally duplicating a physical/other desktop output.
        if (target && target->capture_kind == DisplayCaptureKind::WindowsIddSwapchain) return false;
#else
        (void)target;
#endif
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
