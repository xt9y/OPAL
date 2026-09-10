#include <opal/capture_backend.hpp>

#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <mach/mach_time.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace opal {
namespace {
using Clock = std::chrono::steady_clock;
std::uint64_t monotonic_us()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now().time_since_epoch()).count());
}

std::uint64_t mach_delta_us(std::uint64_t ticks)
{
    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t info{};
        (void)mach_timebase_info(&info);
        return info;
    }();
    if (timebase.denom == 0) return 0;
    const long double nanoseconds = static_cast<long double>(ticks) *
                                    static_cast<long double>(timebase.numer) /
                                    static_cast<long double>(timebase.denom);
    return nanoseconds > 0.0L ? static_cast<std::uint64_t>(nanoseconds / 1000.0L) : 0;
}

PlatformError apple_error(PlatformComponent component, NSError *error, std::string fallback)
{
    PlatformError result;
    result.component = component;
    result.failure = PlatformFailure::OsError;
    result.message = error ? [[error localizedDescription] UTF8String] : std::move(fallback);
    std::string lower = result.message;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){return static_cast<char>(std::tolower(c));});
    if (lower.find("permission") != std::string::npos || lower.find("denied") != std::string::npos ||
        lower.find("privacy") != std::string::npos) {
        result.failure = PlatformFailure::PermissionDenied;
    }
    return result;
}

SCDisplay *retain_display(SCShareableContent *shareable, CGDirectDisplayID requested_id)
{
    if (!shareable || shareable.displays.count == 0) return nil;
    const CGDirectDisplayID wanted = requested_id != 0 ? requested_id : CGMainDisplayID();
    for (SCDisplay *candidate in shareable.displays) {
        if (candidate.displayID == wanted) return [candidate retain];
    }
    if (requested_id != 0) return nil;
    return [shareable.displays.firstObject retain];
}
}
}

static void opal_screen_capture_accept(void *owner, CMSampleBufferRef sample);
static void opal_screen_capture_stopped(void *owner, NSError *error);

@interface OpalScreenStreamOutput : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic, assign) void *owner;
@end

@implementation OpalScreenStreamOutput
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type
{
    (void)stream;
    if (type == SCStreamOutputTypeScreen && self.owner) opal_screen_capture_accept(self.owner, sampleBuffer);
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
    (void)stream;
    if (self.owner) opal_screen_capture_stopped(self.owner, error);
}
@end

namespace opal {
namespace {

class MacCaptureBackend final : public CaptureBackend {
public:
    ~MacCaptureBackend() override { stop(); }

    bool start(const StreamOptions& stream) override
    {
        return start(stream, nullptr);
    }

    bool start(const StreamOptions& stream, const DisplayTarget* target) override
    {
        stop();
        {
            std::lock_guard<std::mutex> lock(mu_);
            error_ = {};
            timestamp_quality_ = CaptureTimestampQuality::Estimated;
        }
        const int requested_fps = std::clamp(stream.fps, 15, 240);
        const CGDirectDisplayID requested_display =
            target && target->capture_kind == DisplayCaptureKind::NativeDisplay && target->native_id != 0
                ? static_cast<CGDirectDisplayID>(target->native_id)
                : 0;

        __block SCShareableContent *shareable = nil;
        __block NSError *share_error = nil;
        dispatch_semaphore_t content_sem = dispatch_semaphore_create(0);
        [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                               onScreenWindowsOnly:NO
                                                 completionHandler:^(SCShareableContent *content, NSError *error) {
            shareable = [content retain];
            share_error = [error retain];
            dispatch_semaphore_signal(content_sem);
        }];
        const bool content_timeout = dispatch_semaphore_wait(content_sem, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) != 0;
        if (!content_timeout) dispatch_release(content_sem);
        if (content_timeout) {
            std::lock_guard<std::mutex> lock(mu_);
            error_ = {PlatformComponent::Capture, PlatformFailure::Unavailable,
                      "ScreenCaptureKit shareable-content request timed out", false};
            return false;
        }
        if (share_error || !shareable || shareable.displays.count == 0) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                error_ = apple_error(PlatformComponent::Capture, share_error, "ScreenCaptureKit found no capturable display");
            }
            [share_error release];
            [shareable release];
            return false;
        }

        SCDisplay *display = retain_display(shareable, requested_display);
        [shareable release];
        [share_error release];
        if (!display) {
            std::lock_guard<std::mutex> lock(mu_);
            error_ = {PlatformComponent::Capture, PlatformFailure::Unavailable,
                      requested_display != 0 ? "ScreenCaptureKit could not resolve the OPAL virtual display"
                                             : "ScreenCaptureKit display unavailable", false};
            return false;
        }

        selected_display_id_ = display.displayID;
        int width = static_cast<int>(display.width);
        int height = static_cast<int>(display.height);
        if (stream.max_width > 0 && stream.max_height > 0 && width > 0 && height > 0) {
            const double scale = std::min({1.0,
                static_cast<double>(stream.max_width) / static_cast<double>(width),
                static_cast<double>(stream.max_height) / static_cast<double>(height)});
            width = std::max(16, static_cast<int>(std::lround(width * scale)));
            height = std::max(16, static_cast<int>(std::lround(height * scale)));
            width &= ~1;
            height &= ~1;
        }

        SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
        [display release];
        SCStreamConfiguration *configuration = [[SCStreamConfiguration alloc] init];
        configuration.width = std::max(16, width);
        configuration.height = std::max(16, height);
        configuration.minimumFrameInterval = CMTimeMake(1, requested_fps);
        // Remote desktop cares about the newest frame, never capture backlog.
        // A deeper ScreenCaptureKit queue can turn a transient encoder stall
        // into whole frames of glass-to-glass latency.
        configuration.queueDepth = 1;
        configuration.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        configuration.showsCursor = YES;
        configuration.scalesToFit = YES;

        output_ = [[OpalScreenStreamOutput alloc] init];
        output_.owner = this;
        queue_ = dispatch_queue_create("de.xt9y.opal.capture.video", DISPATCH_QUEUE_SERIAL);
        stream_ = [[SCStream alloc] initWithFilter:filter configuration:configuration delegate:output_];
        [filter release];
        [configuration release];

        NSError *add_error = nil;
        if (![stream_ addStreamOutput:output_ type:SCStreamOutputTypeScreen sampleHandlerQueue:queue_ error:&add_error]) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                error_ = apple_error(PlatformComponent::Capture, add_error, "ScreenCaptureKit could not attach screen output");
            }
            stop();
            return false;
        }

        __block NSError *start_error = nil;
        dispatch_semaphore_t start_sem = dispatch_semaphore_create(0);
        [stream_ startCaptureWithCompletionHandler:^(NSError *error) {
            start_error = [error retain];
            dispatch_semaphore_signal(start_sem);
        }];
        const bool start_timeout = dispatch_semaphore_wait(start_sem, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) != 0;
        if (!start_timeout) dispatch_release(start_sem);
        if (start_timeout) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                error_ = {PlatformComponent::Capture, PlatformFailure::Unavailable,
                          "ScreenCaptureKit start timed out", false};
            }
            stop();
            return false;
        }
        if (start_error) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                error_ = apple_error(PlatformComponent::Capture, start_error, "ScreenCaptureKit failed to start");
            }
            [start_error release];
            stop();
            return false;
        }
        [start_error release];

        {
            std::lock_guard<std::mutex> lock(mu_);
            running_ = true;
        }
        return true;
    }

    bool next(NativeVideoFrame &frame, int timeout_ms) override
    {
        std::unique_lock<std::mutex> lock(mu_);
        if (!latest_.valid()) {
            if (timeout_ms <= 0) return false;
            cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&]{return latest_.valid() || !running_;});
        }
        if (!latest_.valid()) return false;
        frame = std::move(latest_);
        latest_ = {};
        return true;
    }

    void stop() override
    {
        {
            std::lock_guard<std::mutex> lock(mu_);
            running_ = false;
            latest_ = {};
        }
        cv_.notify_all();
        if (output_) output_.owner = nullptr;
        if (stream_) {
            dispatch_semaphore_t stop_sem = dispatch_semaphore_create(0);
            [stream_ stopCaptureWithCompletionHandler:^(NSError *) { dispatch_semaphore_signal(stop_sem); }];
            const bool stop_timeout = dispatch_semaphore_wait(stop_sem, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)) != 0;
            if (!stop_timeout) dispatch_release(stop_sem);
            if (output_) {
                NSError *remove_error = nil;
                (void)[stream_ removeStreamOutput:output_ type:SCStreamOutputTypeScreen error:&remove_error];
            }
        }
        if (queue_) {
            dispatch_sync(queue_, ^{});
            dispatch_release(queue_);
            queue_ = nullptr;
        }
        [stream_ release]; stream_ = nil;
        [output_ release]; output_ = nil;
        selected_display_id_ = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            timestamp_quality_ = CaptureTimestampQuality::Estimated;
        }
    }

    CaptureTimestampQuality timestamp_quality() const override
    {
        std::lock_guard<std::mutex> lock(mu_);
        return timestamp_quality_;
    }

    std::string backend_name() const override
    {
        return selected_display_id_ != 0 ? "screencapturekit-explicit-display" : "screencapturekit";
    }

    PlatformError last_platform_error() const override
    {
        std::lock_guard<std::mutex> lock(mu_);
        return error_;
    }

    void accept(CMSampleBufferRef sample)
    {
        if (!sample || !CMSampleBufferIsValid(sample) || !CMSampleBufferDataIsReady(sample)) return;
        CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample);
        if (!image) return;

        const std::uint64_t callback_us = monotonic_us();
        std::uint64_t capture_us = callback_us;
        CaptureTimestampQuality quality = CaptureTimestampQuality::Estimated;
        CFArrayRef attachments_ref = CMSampleBufferGetSampleAttachmentsArray(sample, false);
        if (attachments_ref && CFArrayGetCount(attachments_ref) > 0) {
            CFDictionaryRef attachment = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments_ref, 0));
            CFNumberRef status = static_cast<CFNumberRef>(CFDictionaryGetValue(attachment, SCStreamFrameInfoStatus));
            if (status) {
                int value = SCFrameStatusComplete;
                if (CFNumberGetValue(status, kCFNumberIntType, &value) && value != SCFrameStatusComplete) return;
            }
            CFNumberRef display_time = static_cast<CFNumberRef>(CFDictionaryGetValue(attachment, SCStreamFrameInfoDisplayTime));
            std::uint64_t display_ticks = 0;
            if (display_time && CFNumberGetValue(display_time, kCFNumberSInt64Type, &display_ticks) && display_ticks != 0) {
                const std::uint64_t now_ticks = mach_absolute_time();
                if (now_ticks >= display_ticks) {
                    const std::uint64_t age_us = mach_delta_us(now_ticks - display_ticks);
                    if (age_us <= callback_us) {
                        capture_us = callback_us - age_us;
                        quality = CaptureTimestampQuality::Exact;
                    }
                }
            }
        }

        CVPixelBufferRef pixel = static_cast<CVPixelBufferRef>(image);
        CVPixelBufferRetain(pixel);
        NativeVideoFrame frame;
        frame.kind = NativeVideoFrameKind::Opaque;
        frame.width = static_cast<int>(CVPixelBufferGetWidth(pixel));
        frame.height = static_cast<int>(CVPixelBufferGetHeight(pixel));
        frame.pixel_format = CVPixelBufferGetPixelFormatType(pixel);
        frame.capture_time_us = capture_us;
        frame.opaque = pixel;
        frame.owner = std::shared_ptr<void>(pixel, [](void *value){CVPixelBufferRelease(static_cast<CVPixelBufferRef>(value));});

        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!running_) return;
            latest_ = std::move(frame);
            timestamp_quality_ = quality;
        }
        cv_.notify_one();
    }

    void stopped(NSError *error)
    {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!running_) return;
            error_ = apple_error(PlatformComponent::Capture, error, "ScreenCaptureKit stream stopped unexpectedly");
            error_.fallback_possible = true;
            running_ = false;
            latest_ = {};
        }
        cv_.notify_all();
    }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    NativeVideoFrame latest_{};
    bool running_ = false;
    CaptureTimestampQuality timestamp_quality_ = CaptureTimestampQuality::Estimated;
    PlatformError error_{};
    SCStream *stream_ = nil;
    OpalScreenStreamOutput *output_ = nil;
    dispatch_queue_t queue_ = nullptr;
    CGDirectDisplayID selected_display_id_ = 0;
};

}

std::unique_ptr<CaptureBackend> make_capture_backend()
{
    return std::make_unique<MacCaptureBackend>();
}

}

static void opal_screen_capture_accept(void *owner, CMSampleBufferRef sample)
{
    static_cast<opal::MacCaptureBackend *>(owner)->accept(sample);
}

static void opal_screen_capture_stopped(void *owner, NSError *error)
{
    static_cast<opal::MacCaptureBackend *>(owner)->stopped(error);
}
