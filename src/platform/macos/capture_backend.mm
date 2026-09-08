#include <opal/capture_backend.hpp>

#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <chrono>
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
}
}

static void opal_screen_capture_accept(void *owner, CMSampleBufferRef sample);

@interface OpalScreenStreamOutput : NSObject <SCStreamOutput>
@property(nonatomic, assign) void *owner;
@end

@implementation OpalScreenStreamOutput
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type
{
    (void)stream;
    if (type == SCStreamOutputTypeScreen && self.owner) opal_screen_capture_accept(self.owner, sampleBuffer);
}
@end

namespace opal {
namespace {

class MacCaptureBackend final : public CaptureBackend {
public:
    ~MacCaptureBackend() override { stop(); }

    bool start(const StreamOptions &stream) override
    {
        stop();
        error_ = {};
        timestamp_quality_ = CaptureTimestampQuality::Estimated;
        const int requested_fps = std::clamp(stream.fps, 15, 240);

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
        if (dispatch_semaphore_wait(content_sem, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) != 0) {
            error_ = {PlatformComponent::Capture, PlatformFailure::Unavailable,
                      "ScreenCaptureKit shareable-content request timed out", false};
            return false;
        }
        if (share_error || !shareable || shareable.displays.count == 0) {
            error_ = apple_error(PlatformComponent::Capture, share_error, "ScreenCaptureKit found no capturable display");
            [share_error release];
            [shareable release];
            return false;
        }

        SCDisplay *display = [shareable.displays.firstObject retain];
        [shareable release];
        [share_error release];
        if (!display) {
            error_ = {PlatformComponent::Capture, PlatformFailure::Unavailable, "ScreenCaptureKit display unavailable", false};
            return false;
        }

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
        configuration.queueDepth = 3;
        configuration.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        configuration.showsCursor = YES;
        configuration.scalesToFit = YES;

        output_ = [[OpalScreenStreamOutput alloc] init];
        output_.owner = this;
        queue_ = dispatch_queue_create("de.xt9y.opal.capture.video", DISPATCH_QUEUE_SERIAL);
        stream_ = [[SCStream alloc] initWithFilter:filter configuration:configuration delegate:nil];
        [filter release];
        [configuration release];

        NSError *add_error = nil;
        if (![stream_ addStreamOutput:output_ type:SCStreamOutputTypeScreen sampleHandlerQueue:queue_ error:&add_error]) {
            error_ = apple_error(PlatformComponent::Capture, add_error, "ScreenCaptureKit could not attach screen output");
            stop();
            return false;
        }

        __block NSError *start_error = nil;
        dispatch_semaphore_t start_sem = dispatch_semaphore_create(0);
        [stream_ startCaptureWithCompletionHandler:^(NSError *error) {
            start_error = [error retain];
            dispatch_semaphore_signal(start_sem);
        }];
        if (dispatch_semaphore_wait(start_sem, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) != 0) {
            error_ = {PlatformComponent::Capture, PlatformFailure::Unavailable,
                      "ScreenCaptureKit start timed out", false};
            stop();
            return false;
        }
        if (start_error) {
            error_ = apple_error(PlatformComponent::Capture, start_error, "ScreenCaptureKit failed to start");
            [start_error release];
            stop();
            return false;
        }
        [start_error release];

        running_ = true;
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
            (void)dispatch_semaphore_wait(stop_sem, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
            if (output_) {
                NSError *remove_error = nil;
                (void)[stream_ removeStreamOutput:output_ type:SCStreamOutputTypeScreen error:&remove_error];
            }
        }
        if (queue_) dispatch_sync(queue_, ^{});
        [stream_ release]; stream_ = nil;
        [output_ release]; output_ = nil;
        queue_ = nullptr;
        anchored_ = false;
        anchor_pts_ = kCMTimeInvalid;
        anchor_mono_us_ = 0;
    }

    CaptureTimestampQuality timestamp_quality() const override { return timestamp_quality_; }
    std::string backend_name() const override { return "screencapturekit"; }
    PlatformError last_platform_error() const override { return error_; }

    void accept(CMSampleBufferRef sample)
    {
        if (!sample || !CMSampleBufferIsValid(sample) || !CMSampleBufferDataIsReady(sample)) return;
        CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample);
        if (!image) return;

        CFArrayRef attachments_ref = CMSampleBufferGetSampleAttachmentsArray(sample, false);
        if (attachments_ref && CFArrayGetCount(attachments_ref) > 0) {
            CFDictionaryRef attachment = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments_ref, 0));
            CFNumberRef status = static_cast<CFNumberRef>(CFDictionaryGetValue(attachment, SCStreamFrameInfoStatus));
            if (status) {
                int value = SCFrameStatusComplete;
                if (CFNumberGetValue(status, kCFNumberIntType, &value) && value != SCFrameStatusComplete) return;
            }
        }

        const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
        std::uint64_t capture_us = monotonic_us();
        CaptureTimestampQuality quality = CaptureTimestampQuality::Estimated;
        if (CMTIME_IS_VALID(pts) && !CMTIME_IS_INDEFINITE(pts)) {
            std::lock_guard<std::mutex> lock(mu_);
            if (!anchored_) {
                anchored_ = true;
                anchor_pts_ = pts;
                anchor_mono_us_ = capture_us;
            }
            const CMTime delta = CMTimeSubtract(pts, anchor_pts_);
            const double seconds = CMTimeGetSeconds(delta);
            if (std::isfinite(seconds) && seconds > -1.0) {
                const auto signed_delta = static_cast<std::int64_t>(std::llround(seconds * 1000000.0));
                if (signed_delta >= 0) capture_us = anchor_mono_us_ + static_cast<std::uint64_t>(signed_delta);
                quality = CaptureTimestampQuality::Exact;
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

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    NativeVideoFrame latest_{};
    bool running_ = false;
    bool anchored_ = false;
    CMTime anchor_pts_ = kCMTimeInvalid;
    std::uint64_t anchor_mono_us_ = 0;
    CaptureTimestampQuality timestamp_quality_ = CaptureTimestampQuality::Estimated;
    PlatformError error_{};
    SCStream *stream_ = nil;
    OpalScreenStreamOutput *output_ = nil;
    dispatch_queue_t queue_ = nullptr;
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
