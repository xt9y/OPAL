#include <opal/native_video_pipeline.hpp>

#include <cassert>
#include <deque>
#include <memory>
#include <string>

namespace {

class FakeCapture final : public opal::CaptureBackend {
public:
    bool start(const opal::StreamOptions&) override { started = true; error = {}; return true; }
    bool next(opal::NativeVideoFrame& frame, int) override {
        if (frames.empty()) return false;
        frame = std::move(frames.front());
        frames.pop_front();
        return true;
    }
    void stop() override { started = false; }
    opal::CaptureTimestampQuality timestamp_quality() const override { return opal::CaptureTimestampQuality::Exact; }
    std::string backend_name() const override { return "fake-capture"; }
    opal::PlatformError last_platform_error() const override { return error; }

    bool started = false;
    std::deque<opal::NativeVideoFrame> frames;
    opal::PlatformError error{};
};

class FakeEncoder final : public opal::VideoEncoderBackend {
public:
    bool start(const opal::StreamOptions&, int bitrate) override { started = true; bitrate_kbps = bitrate; error = {}; return true; }
    bool encode(const opal::NativeVideoFrame& frame, opal::EncodedMediaUnit& unit) override {
        ++encode_calls;
        if (fail_encode) {
            error = {opal::PlatformComponent::Encoder, opal::PlatformFailure::Unavailable,
                     "fake encoder failure", true};
            return false;
        }
        unit.kind = opal::MediaKind::VideoH264;
        unit.data = {0,0,0,1,0x65,static_cast<std::uint8_t>(encode_calls)};
        unit.capture_time_us = frame.capture_time_us;
        unit.pts_us = static_cast<std::int64_t>(frame.capture_time_us);
        unit.keyframe = encode_calls == 1 || force_idr;
        force_idr = false;
        if (config_.extradata.empty()) config_.extradata = {0,0,0,1,0x67,1,0,0,0,1,0x68,1};
        error = {};
        return true;
    }
    void request_idr() override { force_idr = true; }
    bool set_bitrate(int bitrate) override { bitrate_kbps = bitrate; return true; }
    opal::MediaConfig config() const override { return config_; }
    std::string backend_name() const override { return "fake-encoder"; }
    opal::PlatformError last_platform_error() const override { return error; }
    void stop() override { started = false; }

    bool started = false;
    bool force_idr = false;
    bool fail_encode = false;
    int bitrate_kbps = 0;
    int encode_calls = 0;
    opal::MediaConfig config_{};
    opal::PlatformError error{};
};

}

int main()
{
    auto capture = std::make_unique<FakeCapture>();
    auto encoder = std::make_unique<FakeEncoder>();
    auto* capture_ptr = capture.get();
    auto* encoder_ptr = encoder.get();

    opal::NativeVideoPipeline pipeline(std::move(capture), std::move(encoder));
    opal::StreamOptions stream; stream.fps = 120;
    assert(pipeline.start(stream, 24000));
    assert(capture_ptr->started && encoder_ptr->started);
    assert(encoder_ptr->bitrate_kbps == 24000);
    assert(!pipeline.ended());

    opal::NativeVideoFrame first; first.width = 1920; first.height = 1080; first.capture_time_us = 123456;
    capture_ptr->frames.push_back(first);
    opal::EncodedMediaUnit unit;
    assert(pipeline.next(unit, 0));
    assert(unit.capture_time_us == 123456);
    assert(unit.keyframe);
    assert(pipeline.config_revision() == 1);
    assert(!pipeline.config().extradata.empty());
    assert(pipeline.timestamp_quality() == opal::CaptureTimestampQuality::Exact);
    assert(pipeline.backend_name() == "fake-capture+fake-encoder");
    assert(!pipeline.ended());

    assert(pipeline.set_bitrate(12000));
    assert(encoder_ptr->bitrate_kbps == 12000);
    assert(encoder_ptr->encode_calls == 1);

    pipeline.request_idr();
    opal::NativeVideoFrame second; second.width = 1920; second.height = 1080; second.capture_time_us = 223456;
    capture_ptr->frames.push_back(second);
    assert(pipeline.next(unit, 0));
    assert(unit.keyframe);
    assert(encoder_ptr->encode_calls == 2);

    // A normal capture timeout is not terminal.
    assert(!pipeline.next(unit, 0));
    assert(!pipeline.ended());

    // A capture backend error terminates the current media generation.
    capture_ptr->error = {opal::PlatformComponent::Capture, opal::PlatformFailure::OsError,
                          "fake capture stopped", true};
    assert(!pipeline.next(unit, 0));
    assert(pipeline.ended());
    assert(pipeline.last_platform_error().message == "fake capture stopped");

    pipeline.stop();
    assert(!capture_ptr->started && !encoder_ptr->started);
    assert(!pipeline.ended());

    // Encoder failures also terminate the generation so VideoSender can restart.
    auto capture2 = std::make_unique<FakeCapture>();
    auto encoder2 = std::make_unique<FakeEncoder>();
    auto* capture2_ptr = capture2.get();
    auto* encoder2_ptr = encoder2.get();
    opal::NativeVideoPipeline encoder_failure(std::move(capture2), std::move(encoder2));
    assert(encoder_failure.start(stream, 24000));
    encoder2_ptr->fail_encode = true;
    opal::NativeVideoFrame third; third.width = 1920; third.height = 1080; third.capture_time_us = 323456;
    capture2_ptr->frames.push_back(third);
    assert(!encoder_failure.next(unit, 0));
    assert(encoder_failure.ended());
    assert(encoder_failure.last_platform_error().message == "fake encoder failure");

    return 0;
}
