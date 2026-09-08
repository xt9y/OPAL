#include <opal/audio_capture_backend.hpp>

#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <AudioToolbox/AudioToolbox.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace opal {
namespace {
using Clock = std::chrono::steady_clock;

std::uint64_t monotonic_us()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now().time_since_epoch()).count());
}

PlatformError mac_audio_error(NSError* error, std::string fallback)
{
    PlatformError out;
    out.component = PlatformComponent::AudioCapture;
    out.failure = PlatformFailure::OsError;
    out.message = error ? [[error localizedDescription] UTF8String] : std::move(fallback);
    std::string lower = out.message;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    if (lower.find("permission") != std::string::npos || lower.find("denied") != std::string::npos ||
        lower.find("privacy") != std::string::npos) out.failure = PlatformFailure::PermissionDenied;
    return out;
}

AVSampleFormat pcm_format(const AudioStreamBasicDescription& asbd)
{
    if (asbd.mFormatID != kAudioFormatLinearPCM) return AV_SAMPLE_FMT_NONE;
    const bool planar = (asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
    if (asbd.mFormatFlags & kAudioFormatFlagIsFloat) {
        if (asbd.mBitsPerChannel == 32) return planar ? AV_SAMPLE_FMT_FLTP : AV_SAMPLE_FMT_FLT;
        if (asbd.mBitsPerChannel == 64) return planar ? AV_SAMPLE_FMT_DBLP : AV_SAMPLE_FMT_DBL;
    }
    if (asbd.mFormatFlags & kAudioFormatFlagIsSignedInteger) {
        if (asbd.mBitsPerChannel == 16) return planar ? AV_SAMPLE_FMT_S16P : AV_SAMPLE_FMT_S16;
        if (asbd.mBitsPerChannel == 32) return planar ? AV_SAMPLE_FMT_S32P : AV_SAMPLE_FMT_S32;
    }
    return AV_SAMPLE_FMT_NONE;
}

SCDisplay* retain_main_display(SCShareableContent* shareable)
{
    if (!shareable || shareable.displays.count == 0) return nil;
    const CGDirectDisplayID main_id = CGMainDisplayID();
    for (SCDisplay* candidate in shareable.displays) {
        if (candidate.displayID == main_id) return [candidate retain];
    }
    return [shareable.displays.firstObject retain];
}
}
}

static void opal_audio_capture_accept(void* owner, CMSampleBufferRef sample);

@interface OpalAudioStreamOutput : NSObject <SCStreamOutput>
@property(nonatomic, assign) void* owner;
@end

@implementation OpalAudioStreamOutput
- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type
{
    (void)stream;
    if (type == SCStreamOutputTypeAudio && self.owner) opal_audio_capture_accept(self.owner, sampleBuffer);
}
@end

namespace opal {
namespace {

class MacAudioCaptureBackend final : public AudioCaptureBackend {
public:
    ~MacAudioCaptureBackend() override { stop(); }

    bool start() override
    {
        stop();
        error_ = {};

        __block SCShareableContent* shareable = nil;
        __block NSError* share_error = nil;
        dispatch_semaphore_t content_sem = dispatch_semaphore_create(0);
        [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                               onScreenWindowsOnly:NO
                                                 completionHandler:^(SCShareableContent* content, NSError* error) {
            shareable = [content retain];
            share_error = [error retain];
            dispatch_semaphore_signal(content_sem);
        }];
        if (dispatch_semaphore_wait(content_sem, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) != 0) {
            error_ = {PlatformComponent::AudioCapture, PlatformFailure::Unavailable,
                      "ScreenCaptureKit audio shareable-content request timed out", false};
            return false;
        }
        if (share_error || !shareable || shareable.displays.count == 0) {
            error_ = mac_audio_error(share_error, "ScreenCaptureKit found no display for system audio capture");
            [share_error release];
            [shareable release];
            return false;
        }

        SCDisplay* display = retain_main_display(shareable);
        [share_error release];
        [shareable release];
        if (!display) {
            error_ = {PlatformComponent::AudioCapture, PlatformFailure::Unavailable,
                      "ScreenCaptureKit audio display unavailable", false};
            return false;
        }

        SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
        [display release];
        SCStreamConfiguration* configuration = [[SCStreamConfiguration alloc] init];
        configuration.capturesAudio = YES;
        configuration.sampleRate = 48000;
        configuration.channelCount = 2;
        configuration.excludesCurrentProcessAudio = YES;

        output_ = [[OpalAudioStreamOutput alloc] init];
        output_.owner = this;
        queue_ = dispatch_queue_create("de.xt9y.opal.capture.audio", DISPATCH_QUEUE_SERIAL);
        stream_ = [[SCStream alloc] initWithFilter:filter configuration:configuration delegate:nil];
        [filter release];
        [configuration release];

        NSError* add_error = nil;
        if (![stream_ addStreamOutput:output_ type:SCStreamOutputTypeAudio sampleHandlerQueue:queue_ error:&add_error]) {
            error_ = mac_audio_error(add_error, "ScreenCaptureKit could not attach system audio output");
            stop();
            return false;
        }

        __block NSError* start_error = nil;
        dispatch_semaphore_t start_sem = dispatch_semaphore_create(0);
        [stream_ startCaptureWithCompletionHandler:^(NSError* error) {
            start_error = [error retain];
            dispatch_semaphore_signal(start_sem);
        }];
        if (dispatch_semaphore_wait(start_sem, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) != 0) {
            error_ = {PlatformComponent::AudioCapture, PlatformFailure::Unavailable,
                      "ScreenCaptureKit audio start timed out", false};
            stop();
            return false;
        }
        if (start_error) {
            error_ = mac_audio_error(start_error, "ScreenCaptureKit system audio capture failed to start");
            [start_error release];
            stop();
            return false;
        }
        [start_error release];
        running_ = true;
        return true;
    }

    bool next(EncodedMediaUnit& unit, int timeout_ms) override
    {
        unit = {};
        if (!running_) return false;
        const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(0, timeout_ms));
        for (;;) {
            if (fifo_ && codec_ && codec_->frame_size > 0 && av_audio_fifo_size(fifo_) >= codec_->frame_size) {
                if (encode_one(unit)) return true;
                if (error_) return false;
            }

            std::shared_ptr<void> sample_owner;
            {
                std::unique_lock<std::mutex> lock(mu_);
                if (!latest_sample_) {
                    if (timeout_ms <= 0) return false;
                    const auto now = Clock::now();
                    if (now >= deadline) return false;
                    cv_.wait_until(lock, deadline, [&]{ return latest_sample_ || !running_; });
                }
                if (!running_ || !latest_sample_) return false;
                sample_owner = std::move(latest_sample_);
            }
            if (!ingest(static_cast<CMSampleBufferRef>(sample_owner.get()))) return false;
        }
    }

    void stop() override
    {
        {
            std::lock_guard<std::mutex> lock(mu_);
            running_ = false;
            latest_sample_.reset();
        }
        cv_.notify_all();
        if (output_) output_.owner = nullptr;
        if (stream_) {
            dispatch_semaphore_t stop_sem = dispatch_semaphore_create(0);
            [stream_ stopCaptureWithCompletionHandler:^(NSError*) { dispatch_semaphore_signal(stop_sem); }];
            (void)dispatch_semaphore_wait(stop_sem, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
            if (output_) {
                NSError* remove_error = nil;
                (void)[stream_ removeStreamOutput:output_ type:SCStreamOutputTypeAudio error:&remove_error];
            }
        }
        if (queue_) dispatch_sync(queue_, ^{});
        [stream_ release]; stream_ = nil;
        [output_ release]; output_ = nil;
        queue_ = nullptr;
        reset_encoder();
        config_ = {};
        config_revision_ = 0;
        fifo_capture_us_ = 0;
        samples_encoded_ = 0;
        audio_anchor_valid_ = false;
        audio_anchor_pts_ = kCMTimeInvalid;
        audio_anchor_us_ = 0;
    }

    MediaConfig config() const override { return config_; }
    std::uint64_t config_revision() const override { return config_revision_; }
    std::string backend_name() const override { return "screencapturekit+aac"; }
    PlatformError last_platform_error() const override { return error_; }

    void accept(CMSampleBufferRef sample)
    {
        if (!sample || !CMSampleBufferIsValid(sample) || !CMSampleBufferDataIsReady(sample)) return;
        CFRetain(sample);
        auto owner = std::shared_ptr<void>(const_cast<void*>(reinterpret_cast<const void*>(sample)),
            [](void* value){ if (value) CFRelease(static_cast<CFTypeRef>(value)); });
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!running_) return;
            latest_sample_ = std::move(owner);
        }
        cv_.notify_one();
    }

private:
    bool open_encoder(int sample_rate, int channels, AVSampleFormat input_format)
    {
        reset_encoder();
        const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!encoder) {
            error_ = {PlatformComponent::AudioCapture, PlatformFailure::DependencyMissing,
                      "linked libavcodec has no AAC encoder", false};
            return false;
        }
        codec_ = avcodec_alloc_context3(encoder);
        if (!codec_) return false;
        codec_->sample_rate = sample_rate;
        av_channel_layout_default(&codec_->ch_layout, channels);
        codec_->time_base = AVRational{1, sample_rate};
        codec_->bit_rate = 128000;
        codec_->flags |= AV_CODEC_FLAG_LOW_DELAY | AV_CODEC_FLAG_GLOBAL_HEADER;
        const AVSampleFormat* sample_formats = nullptr;
        if (avcodec_get_supported_config(codec_, nullptr, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                         reinterpret_cast<const void**>(&sample_formats), nullptr) >= 0 && sample_formats)
            codec_->sample_fmt = sample_formats[0];
        else
            codec_->sample_fmt = AV_SAMPLE_FMT_FLTP;
        if (avcodec_open2(codec_, encoder, nullptr) < 0) {
            error_ = {PlatformComponent::AudioCapture, PlatformFailure::Unavailable,
                      "could not open persistent AAC encoder", false};
            reset_encoder();
            return false;
        }
        if (codec_->frame_size <= 0) {
            error_ = {PlatformComponent::AudioCapture, PlatformFailure::Unsupported,
                      "AAC encoder did not expose a fixed low-latency frame size", false};
            reset_encoder();
            return false;
        }
        fifo_ = av_audio_fifo_alloc(codec_->sample_fmt, channels, std::max(4096, codec_->frame_size * 4));
        if (!fifo_) {
            reset_encoder();
            return false;
        }
        AVChannelLayout input_layout{};
        av_channel_layout_default(&input_layout, channels);
        if (swr_alloc_set_opts2(&swr_, &codec_->ch_layout, codec_->sample_fmt, sample_rate,
                                &input_layout, input_format, sample_rate, 0, nullptr) < 0 ||
            !swr_ || swr_init(swr_) < 0) {
            av_channel_layout_uninit(&input_layout);
            error_ = {PlatformComponent::AudioCapture, PlatformFailure::Unavailable,
                      "could not initialize system-audio resampler", false};
            reset_encoder();
            return false;
        }
        av_channel_layout_uninit(&input_layout);

        input_format_ = input_format;
        input_rate_ = sample_rate;
        input_channels_ = channels;
        config_ = {};
        config_.kind = MediaKind::AudioAac;
        config_.sample_rate = sample_rate;
        config_.channels = channels;
        if (codec_->extradata && codec_->extradata_size > 0)
            config_.extradata.assign(codec_->extradata, codec_->extradata + codec_->extradata_size);
        ++config_revision_;
        if (config_.extradata.empty()) {
            error_ = {PlatformComponent::AudioCapture, PlatformFailure::Unavailable,
                      "AAC encoder did not provide AudioSpecificConfig", false};
            reset_encoder();
            return false;
        }
        return true;
    }

    void reset_encoder()
    {
        if (fifo_) av_audio_fifo_free(fifo_);
        fifo_ = nullptr;
        if (swr_) swr_free(&swr_);
        if (codec_) avcodec_free_context(&codec_);
        input_format_ = AV_SAMPLE_FMT_NONE;
        input_rate_ = 0;
        input_channels_ = 0;
        fifo_capture_us_ = 0;
    }

    bool ingest(CMSampleBufferRef sample)
    {
        if (!sample) return false;
        auto* description = CMAudioFormatDescriptionGetStreamBasicDescription(
            static_cast<CMAudioFormatDescriptionRef>(CMSampleBufferGetFormatDescription(sample)));
        if (!description) return false;
        const AVSampleFormat format = pcm_format(*description);
        const int sample_rate = static_cast<int>(description->mSampleRate);
        const int channels = static_cast<int>(description->mChannelsPerFrame);
        if (format == AV_SAMPLE_FMT_NONE || sample_rate <= 0 || channels <= 0 || channels > 8) {
            error_ = {PlatformComponent::AudioCapture, PlatformFailure::Unsupported,
                      "unsupported ScreenCaptureKit PCM format", false};
            return false;
        }
        if (!codec_ || format != input_format_ || sample_rate != input_rate_ || channels != input_channels_) {
            if (!open_encoder(sample_rate, channels, format)) return false;
        }

        const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
        const auto now_us = monotonic_us();
        std::uint64_t sample_capture_us = now_us;
        if (CMTIME_IS_VALID(pts) && !CMTIME_IS_INDEFINITE(pts) && !audio_anchor_valid_) {
            audio_anchor_valid_ = true;
            audio_anchor_pts_ = pts;
            audio_anchor_us_ = now_us;
        } else if (audio_anchor_valid_ && CMTIME_IS_VALID(pts) && !CMTIME_IS_INDEFINITE(pts)) {
            const double seconds = CMTimeGetSeconds(CMTimeSubtract(pts, audio_anchor_pts_));
            if (std::isfinite(seconds) && seconds >= 0.0)
                sample_capture_us = audio_anchor_us_ + static_cast<std::uint64_t>(seconds * 1000000.0);
        }

        size_t list_size = 0;
        if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample, &list_size, nullptr, 0,
                kCFAllocatorDefault, kCFAllocatorDefault,
                kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, nullptr) != noErr || list_size == 0)
            return false;
        std::vector<std::uint8_t> list_storage(list_size);
        auto* list = reinterpret_cast<AudioBufferList*>(list_storage.data());
        CMBlockBufferRef block = nullptr;
        if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample, nullptr, list, list_size,
                kCFAllocatorDefault, kCFAllocatorDefault,
                kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &block) != noErr) return false;

        const int input_samples = static_cast<int>(CMSampleBufferGetNumSamples(sample));
        std::vector<const std::uint8_t*> input(static_cast<std::size_t>(std::max(1, channels)), nullptr);
        for (UInt32 i = 0; i < list->mNumberBuffers && i < input.size(); ++i)
            input[i] = static_cast<const std::uint8_t*>(list->mBuffers[i].mData);
        if (!input[0] || input_samples <= 0) { if (block) CFRelease(block); return false; }

        const int output_capacity = static_cast<int>(av_rescale_rnd(
            swr_get_delay(swr_, sample_rate) + input_samples, codec_->sample_rate, sample_rate, AV_ROUND_UP));
        AVFrame* converted = av_frame_alloc();
        if (!converted) { if (block) CFRelease(block); return false; }
        converted->format = codec_->sample_fmt;
        converted->sample_rate = codec_->sample_rate;
        converted->nb_samples = std::max(1, output_capacity);
        av_channel_layout_copy(&converted->ch_layout, &codec_->ch_layout);
        if (av_frame_get_buffer(converted, 0) < 0) {
            av_frame_free(&converted); if (block) CFRelease(block); return false;
        }
        const int converted_samples = swr_convert(swr_, converted->data, converted->nb_samples,
                                                  input.data(), input_samples);
        if (block) CFRelease(block);
        if (converted_samples <= 0) { av_frame_free(&converted); return false; }
        const int fifo_samples_before = av_audio_fifo_size(fifo_);
        if (av_audio_fifo_realloc(fifo_, fifo_samples_before + converted_samples) < 0 ||
            av_audio_fifo_write(fifo_, reinterpret_cast<void**>(converted->data), converted_samples) != converted_samples) {
            av_frame_free(&converted); return false;
        }
        av_frame_free(&converted);
        if (fifo_samples_before == 0) fifo_capture_us_ = sample_capture_us;
        return true;
    }

    bool encode_one(EncodedMediaUnit& unit)
    {
        if (!codec_ || !fifo_ || codec_->frame_size <= 0 || av_audio_fifo_size(fifo_) < codec_->frame_size) return false;
        AVFrame* frame = av_frame_alloc();
        AVPacket* packet = av_packet_alloc();
        if (!frame || !packet) { av_frame_free(&frame); av_packet_free(&packet); return false; }
        frame->format = codec_->sample_fmt;
        frame->sample_rate = codec_->sample_rate;
        frame->nb_samples = codec_->frame_size;
        frame->pts = static_cast<std::int64_t>(samples_encoded_);
        av_channel_layout_copy(&frame->ch_layout, &codec_->ch_layout);
        if (av_frame_get_buffer(frame, 0) < 0 ||
            av_audio_fifo_read(fifo_, reinterpret_cast<void**>(frame->data), frame->nb_samples) != frame->nb_samples ||
            avcodec_send_frame(codec_, frame) < 0) {
            av_frame_free(&frame); av_packet_free(&packet); return false;
        }
        av_frame_free(&frame);
        const int rc = avcodec_receive_packet(codec_, packet);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) { av_packet_free(&packet); return false; }
        if (rc < 0) {
            error_ = {PlatformComponent::AudioCapture, PlatformFailure::OsError,
                      "AAC encoder failed to emit a packet", false};
            av_packet_free(&packet); return false;
        }

        unit.kind = MediaKind::AudioAac;
        unit.data.assign(packet->data, packet->data + packet->size);
        unit.pts_us = static_cast<std::int64_t>((samples_encoded_ * 1000000ULL) /
                                               static_cast<std::uint64_t>(codec_->sample_rate));
        unit.capture_time_us = fifo_capture_us_ ? fifo_capture_us_ : monotonic_us();
        unit.keyframe = false;
        samples_encoded_ += static_cast<std::uint64_t>(codec_->frame_size);
        if (fifo_capture_us_)
            fifo_capture_us_ += static_cast<std::uint64_t>(codec_->frame_size) * 1000000ULL /
                                static_cast<std::uint64_t>(codec_->sample_rate);
        if (av_audio_fifo_size(fifo_) == 0) fifo_capture_us_ = 0;
        av_packet_free(&packet);
        return !unit.data.empty();
    }

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::shared_ptr<void> latest_sample_;
    bool running_ = false;
    SCStream* stream_ = nil;
    OpalAudioStreamOutput* output_ = nil;
    dispatch_queue_t queue_ = nullptr;

    AVCodecContext* codec_ = nullptr;
    SwrContext* swr_ = nullptr;
    AVAudioFifo* fifo_ = nullptr;
    AVSampleFormat input_format_ = AV_SAMPLE_FMT_NONE;
    int input_rate_ = 0;
    int input_channels_ = 0;
    MediaConfig config_{};
    std::uint64_t config_revision_ = 0;
    std::uint64_t samples_encoded_ = 0;
    std::uint64_t fifo_capture_us_ = 0;
    bool audio_anchor_valid_ = false;
    CMTime audio_anchor_pts_ = kCMTimeInvalid;
    std::uint64_t audio_anchor_us_ = 0;
    PlatformError error_{};
};

}

std::unique_ptr<AudioCaptureBackend> make_audio_capture_backend()
{
    return std::make_unique<MacAudioCaptureBackend>();
}

}

static void opal_audio_capture_accept(void* owner, CMSampleBufferRef sample)
{
    static_cast<opal::MacAudioCaptureBackend*>(owner)->accept(sample);
}