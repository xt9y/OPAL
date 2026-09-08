#pragma once

#include <opal/platform_error.hpp>
#include <opal/video_capture.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace opal {

class AudioCaptureBackend {
public:
    virtual ~AudioCaptureBackend() = default;
    virtual bool start() = 0;
    virtual bool next(EncodedMediaUnit& unit, int timeout_ms) = 0;
    virtual void stop() = 0;
    virtual MediaConfig config() const = 0;
    virtual std::uint64_t config_revision() const = 0;
    virtual std::string backend_name() const = 0;
    virtual PlatformError last_platform_error() const = 0;
};

std::unique_ptr<AudioCaptureBackend> make_audio_capture_backend();

}
