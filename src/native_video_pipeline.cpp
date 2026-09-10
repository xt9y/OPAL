#include <opal/native_video_pipeline.hpp>

#include <chrono>
#include <utility>

namespace opal {
namespace {

bool same_config(const MediaConfig& a, const MediaConfig& b)
{
    return a.kind == b.kind && a.extradata == b.extradata &&
           a.sample_rate == b.sample_rate && a.channels == b.channels;
}

#if defined(_WIN32)
std::uint64_t monotonic_us()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
#endif

}

NativeVideoPipeline::NativeVideoPipeline(std::unique_ptr<CaptureBackend> capture,
                                         std::unique_ptr<VideoEncoderBackend> encoder)
    : capture_(std::move(capture)), encoder_(std::move(encoder))
{
}

NativeVideoPipeline::~NativeVideoPipeline()
{
    stop();
}

bool NativeVideoPipeline::start(const StreamOptions& stream, int bitrate_kbps, const DisplayTarget* target)
{
    stop();
    terminal_ = false;
    if (!capture_ || !encoder_) return false;
    if (!capture_->start(stream, target)) {
        terminal_ = static_cast<bool>(capture_->last_platform_error());
        return false;
    }
    if (!encoder_->start(stream, bitrate_kbps)) {
        terminal_ = static_cast<bool>(encoder_->last_platform_error());
        capture_->stop();
        return false;
    }
    config_ = {};
    config_revision_ = 0;
    first_frame_ = true;
    running_ = true;

    // The sender starts with an invalid H.264 dependency chain. Force the very
    // first encoded frame to be an IDR so a static desktop cannot sit forever
    // waiting for a later desktop update before the decoder can start.
    encoder_->request_idr();
    return true;
}

bool NativeVideoPipeline::next(EncodedMediaUnit& unit, int timeout_ms)
{
    unit = {};
    if (!running_ || terminal_ || !capture_ || !encoder_) return false;
    NativeVideoFrame frame;
    if (!capture_->next(frame, timeout_ms)) {
        if (capture_->last_platform_error()) {
            terminal_ = true;
            running_ = false;
        }
        return false;
    }
#if defined(_WIN32)
    // Desktop Duplication's first LastPresentTime can describe when unchanged
    // pixels were last presented, not when OPAL acquired the current desktop
    // snapshot. Do not classify that bootstrap snapshot as pipeline backlog.
    if (first_frame_) frame.capture_time_us = monotonic_us();
#endif
    first_frame_ = false;
    if (!encoder_->encode(frame, unit)) {
        if (encoder_->last_platform_error()) {
            terminal_ = true;
            running_ = false;
        }
        return false;
    }
    const auto next_config = encoder_->config();
    if (!next_config.extradata.empty() && !same_config(config_, next_config)) {
        config_ = next_config;
        ++config_revision_;
    }
    return !unit.data.empty();
}

void NativeVideoPipeline::request_idr()
{
    if (encoder_ && running_ && !terminal_) encoder_->request_idr();
}

bool NativeVideoPipeline::set_bitrate(int bitrate_kbps)
{
    if (!encoder_ || !running_ || terminal_) return false;
    if (encoder_->set_bitrate(bitrate_kbps)) return true;
    if (encoder_->last_platform_error()) {
        terminal_ = true;
        running_ = false;
    }
    return false;
}

void NativeVideoPipeline::stop()
{
    running_ = false;
    if (encoder_) encoder_->stop();
    if (capture_) capture_->stop();
    config_ = {};
    config_revision_ = 0;
    terminal_ = false;
    first_frame_ = true;
}

const MediaConfig& NativeVideoPipeline::config() const
{
    return config_;
}

std::uint64_t NativeVideoPipeline::config_revision() const
{
    return config_revision_;
}

CaptureTimestampQuality NativeVideoPipeline::timestamp_quality() const
{
    return capture_ ? capture_->timestamp_quality() : CaptureTimestampQuality::Estimated;
}

std::string NativeVideoPipeline::backend_name() const
{
    if (!capture_ || !encoder_) return "unavailable";
    return capture_->backend_name() + "+" + encoder_->backend_name();
}

PlatformError NativeVideoPipeline::last_platform_error() const
{
    if (encoder_) {
        auto error = encoder_->last_platform_error();
        if (error) return error;
    }
    return capture_ ? capture_->last_platform_error() : PlatformError{};
}

bool NativeVideoPipeline::ended() const noexcept
{
    return terminal_;
}

}
