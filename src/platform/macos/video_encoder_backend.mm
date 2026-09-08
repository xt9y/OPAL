#include <opal/video_encoder_backend.hpp>

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace opal {
namespace {

PlatformError vt_error(OSStatus status, std::string message)
{
    PlatformError error;
    error.component = PlatformComponent::Encoder;
    error.failure = status == kVTVideoEncoderNotAvailableNowErr ? PlatformFailure::Unavailable : PlatformFailure::OsError;
    error.message = std::move(message) + " (OSStatus " + std::to_string(status) + ")";
    return error;
}

bool set_bool(VTCompressionSessionRef session, CFStringRef key, bool value)
{
    return VTSessionSetProperty(session, key, value ? kCFBooleanTrue : kCFBooleanFalse) == noErr;
}

bool set_i32(VTCompressionSessionRef session, CFStringRef key, std::int32_t value)
{
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
    if (!number) return false;
    const OSStatus status = VTSessionSetProperty(session, key, number);
    CFRelease(number);
    return status == noErr;
}

bool keyframe_sample(CMSampleBufferRef sample)
{
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (!attachments || CFArrayGetCount(attachments) == 0) return true;
    CFDictionaryRef dictionary = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
    return !CFDictionaryContainsKey(dictionary, kCMSampleAttachmentKey_NotSync);
}

bool append_parameter_sets(CMFormatDescriptionRef format, MediaConfig &config, std::size_t &nal_length_size)
{
    if (!format) return false;
    const uint8_t *set = nullptr;
    size_t set_size = 0;
    size_t count = 0;
    int header_length = 4;
    OSStatus status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
        static_cast<CMVideoFormatDescriptionRef>(format), 0, &set, &set_size, &count, &header_length);
    if (status != noErr || count < 2 || header_length < 1 || header_length > 4) return false;
    config = {};
    config.kind = MediaKind::VideoH264;
    static constexpr std::array<std::uint8_t,4> start_code{0,0,0,1};
    for (size_t index = 0; index < count; ++index) {
        status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
            static_cast<CMVideoFormatDescriptionRef>(format), index, &set, &set_size, nullptr, nullptr);
        if (status != noErr || !set || set_size == 0) return false;
        config.extradata.insert(config.extradata.end(), start_code.begin(), start_code.end());
        config.extradata.insert(config.extradata.end(), set, set + set_size);
    }
    nal_length_size = static_cast<std::size_t>(header_length);
    return !config.extradata.empty();
}

bool sample_to_annexb(CMSampleBufferRef sample, std::size_t nal_length_size, std::vector<std::uint8_t> &out)
{
    if (!sample || nal_length_size < 1 || nal_length_size > 4) return false;
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    if (!block) return false;
    size_t length_at_offset = 0, total_length = 0;
    char *data = nullptr;
    if (CMBlockBufferGetDataPointer(block, 0, &length_at_offset, &total_length, &data) != kCMBlockBufferNoErr ||
        !data || total_length == 0) return false;

    out.clear();
    out.reserve(total_length + 64);
    static constexpr std::array<std::uint8_t,4> start_code{0,0,0,1};
    std::size_t offset = 0;
    while (offset + nal_length_size <= total_length) {
        std::uint32_t nal_size = 0;
        for (std::size_t i = 0; i < nal_length_size; ++i)
            nal_size = (nal_size << 8) | static_cast<std::uint8_t>(data[offset + i]);
        offset += nal_length_size;
        if (nal_size == 0 || offset + nal_size > total_length) return false;
        out.insert(out.end(), start_code.begin(), start_code.end());
        out.insert(out.end(), reinterpret_cast<std::uint8_t *>(data + offset),
                   reinterpret_cast<std::uint8_t *>(data + offset + nal_size));
        offset += nal_size;
    }
    return offset == total_length && !out.empty();
}

struct EncodeRequest {
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    OSStatus status = noErr;
    bool ok = false;
    EncodedMediaUnit unit{};
    MediaConfig discovered_config{};
    std::size_t discovered_nal_length_size = 0;
    std::shared_ptr<void> pixel_owner;
};

class MacVideoEncoderBackend final : public VideoEncoderBackend {
public:
    ~MacVideoEncoderBackend() override { stop(); }

    bool start(const StreamOptions &stream, int bitrate_kbps) override
    {
        stop();
        stream_ = stream;
        fps_ = std::clamp(stream.fps, 15, 240);
        bitrate_kbps_ = std::max(1000, bitrate_kbps);
        error_ = {};
        return true;
    }

    bool encode(const NativeVideoFrame &frame, EncodedMediaUnit &unit) override
    {
        unit = {};
        if (frame.kind != NativeVideoFrameKind::Opaque || !frame.opaque || frame.width <= 0 || frame.height <= 0) {
            error_ = {PlatformComponent::Encoder, PlatformFailure::InvalidState,
                      "VideoToolbox requires an opaque CVPixelBuffer frame", false};
            return false;
        }
        if (!session_ && !open_session(frame.width, frame.height)) return false;
        if (frame.width != width_ || frame.height != height_) {
            error_ = {PlatformComponent::Encoder, PlatformFailure::InvalidState,
                      "capture dimensions changed; media generation must restart", true};
            return false;
        }

        CVPixelBufferRef pixel = static_cast<CVPixelBufferRef>(frame.opaque);
        const std::uint64_t index = frame_index_++;
        const std::uint64_t capture_time_us = frame.capture_time_us;
        const int fps = fps_;
        const CMTime pts = CMTimeMake(static_cast<std::int64_t>(index), fps);
        const CMTime duration = CMTimeMake(1, fps);
        const bool force_idr = force_idr_.exchange(false, std::memory_order_acq_rel);
        const bool need_config = config_.extradata.empty();
        const std::size_t current_nal_length_size = nal_length_size_;

        CFDictionaryRef frame_properties = nullptr;
        if (force_idr) {
            const void *keys[] = {kVTEncodeFrameOptionKey_ForceKeyFrame};
            const void *values[] = {kCFBooleanTrue};
            frame_properties = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                                                   &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
        }

        auto request = std::make_shared<EncodeRequest>();
        request->pixel_owner = frame.owner;
        VTEncodeInfoFlags flags = 0;
        const OSStatus status = VTCompressionSessionEncodeFrameWithOutputHandler(
            session_, pixel, pts, duration, frame_properties, &flags,
            ^(OSStatus output_status, VTEncodeInfoFlags output_flags, CMSampleBufferRef sample) {
                request->status = output_status;
                std::size_t nal_length = current_nal_length_size;
                if (output_status == noErr && !(output_flags & kVTEncodeInfo_FrameDropped) && sample) {
                    if (need_config) {
                        MediaConfig discovered;
                        if (append_parameter_sets(CMSampleBufferGetFormatDescription(sample), discovered, nal_length)) {
                            request->discovered_config = std::move(discovered);
                            request->discovered_nal_length_size = nal_length;
                        }
                    }
                    request->unit.kind = MediaKind::VideoH264;
                    request->unit.pts_us = static_cast<std::int64_t>((index * 1000000ULL) / static_cast<std::uint64_t>(fps));
                    request->unit.capture_time_us = capture_time_us;
                    request->unit.keyframe = keyframe_sample(sample);
                    request->ok = sample_to_annexb(sample, nal_length, request->unit.data);
                }
                request->pixel_owner.reset();
                dispatch_semaphore_signal(request->done);
            });
        if (frame_properties) CFRelease(frame_properties);
        if (status != noErr) {
            request->pixel_owner.reset();
            error_ = vt_error(status, "VTCompressionSessionEncodeFrame failed");
            return false;
        }
        if (dispatch_semaphore_wait(request->done, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)) != 0) {
            error_ = {PlatformComponent::Encoder, PlatformFailure::Unavailable,
                      "VideoToolbox encode callback timed out", true};
            invalidate_session();
            return false;
        }
        if (!request->ok) {
            error_ = request->status == noErr
                ? PlatformError{PlatformComponent::Encoder, PlatformFailure::Unavailable,
                                "VideoToolbox returned no usable H.264 access unit", true}
                : vt_error(request->status, "VideoToolbox output callback failed");
            return false;
        }
        if (!request->discovered_config.extradata.empty()) {
            config_ = std::move(request->discovered_config);
            nal_length_size_ = request->discovered_nal_length_size;
        }
        unit = std::move(request->unit);
        return true;
    }

    void request_idr() override { force_idr_.store(true, std::memory_order_release); }

    bool set_bitrate(int bitrate_kbps) override
    {
        bitrate_kbps_ = std::max(1000, bitrate_kbps);
        if (!session_) return true;
        const std::int64_t requested_bits = static_cast<std::int64_t>(bitrate_kbps_) * 1000;
        const auto bits = static_cast<std::int32_t>(std::min<std::int64_t>(requested_bits, std::numeric_limits<std::int32_t>::max()));
        if (!set_i32(session_, kVTCompressionPropertyKey_AverageBitRate, bits)) {
            error_ = {PlatformComponent::Encoder, PlatformFailure::OsError,
                      "VideoToolbox rejected bitrate update", false};
            return false;
        }
        return true;
    }

    MediaConfig config() const override { return config_; }
    std::string backend_name() const override { return "videotoolbox-hardware"; }
    PlatformError last_platform_error() const override { return error_; }

    void stop() override
    {
        invalidate_session();
        config_ = {};
        nal_length_size_ = 4;
        width_ = height_ = 0;
        frame_index_ = 0;
        force_idr_.store(false, std::memory_order_release);
    }

private:
    void invalidate_session()
    {
        if (!session_) return;
        VTCompressionSessionInvalidate(session_);
        CFRelease(session_);
        session_ = nullptr;
    }

    bool open_session(int width, int height)
    {
        const void *spec_keys[] = {kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder};
        const void *spec_values[] = {kCFBooleanTrue};
        CFDictionaryRef specification = CFDictionaryCreate(kCFAllocatorDefault, spec_keys, spec_values, 1,
                                                            &kCFTypeDictionaryKeyCallBacks,
                                                            &kCFTypeDictionaryValueCallBacks);
        VTCompressionSessionRef session = nullptr;
        const OSStatus status = VTCompressionSessionCreate(
            kCFAllocatorDefault, width, height, kCMVideoCodecType_H264,
            specification, nullptr, kCFAllocatorDefault, nullptr, nullptr, &session);
        if (specification) CFRelease(specification);
        if (status != noErr || !session) {
            error_ = vt_error(status, "hardware VTCompressionSessionCreate failed");
            error_.failure = PlatformFailure::Unavailable;
            return false;
        }
        session_ = session;
        width_ = width;
        height_ = height;

        const std::int64_t requested_bits = static_cast<std::int64_t>(bitrate_kbps_) * 1000;
        const auto bits = static_cast<std::int32_t>(std::min<std::int64_t>(requested_bits, std::numeric_limits<std::int32_t>::max()));
        const std::int32_t fps = fps_;
        const std::int32_t gop = std::max(1, fps_ * 2);
        const std::int32_t frame_delay = 1;
        bool ok = true;
        ok = ok && set_bool(session_, kVTCompressionPropertyKey_RealTime, true);
        ok = ok && set_bool(session_, kVTCompressionPropertyKey_AllowFrameReordering, false);
        ok = ok && set_i32(session_, kVTCompressionPropertyKey_AverageBitRate, bits);
        ok = ok && set_i32(session_, kVTCompressionPropertyKey_ExpectedFrameRate, fps);
        ok = ok && set_i32(session_, kVTCompressionPropertyKey_MaxKeyFrameInterval, gop);
        ok = ok && set_i32(session_, kVTCompressionPropertyKey_MaxFrameDelayCount, frame_delay);
        (void)VTSessionSetProperty(session_, kVTCompressionPropertyKey_ProfileLevel,
                                   kVTProfileLevel_H264_High_AutoLevel);
        if (!ok) {
            error_ = {PlatformComponent::Encoder, PlatformFailure::OsError,
                      "VideoToolbox rejected required realtime encoder properties", false};
            invalidate_session();
            return false;
        }
        const OSStatus prepare = VTCompressionSessionPrepareToEncodeFrames(session_);
        if (prepare != noErr) {
            error_ = vt_error(prepare, "VTCompressionSessionPrepareToEncodeFrames failed");
            invalidate_session();
            return false;
        }
        return true;
    }

    StreamOptions stream_{};
    VTCompressionSessionRef session_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int fps_ = 60;
    int bitrate_kbps_ = 30000;
    std::uint64_t frame_index_ = 0;
    std::atomic<bool> force_idr_{false};
    std::size_t nal_length_size_ = 4;
    MediaConfig config_{};
    PlatformError error_{};
};

}

std::unique_ptr<VideoEncoderBackend> make_video_encoder_backend()
{
    return std::make_unique<MacVideoEncoderBackend>();
}

}
