#include <opal/audio_capture_backend.hpp>

#include <cassert>
#include <string>

namespace {
class FakeAudio final : public opal::AudioCaptureBackend {
public:
    bool start() override { running = true; ++revision; return true; }
    bool next(opal::EncodedMediaUnit& unit, int timeout_ms) override {
        if (!running || timeout_ms < 0) return false;
        unit.kind = opal::MediaKind::AudioAac;
        unit.data = {1,2,3};
        unit.capture_time_us = 55;
        return true;
    }
    void stop() override { running = false; }
    opal::MediaConfig config() const override {
        opal::MediaConfig c; c.kind = opal::MediaKind::AudioAac; c.extradata = {0x11,0x90}; c.sample_rate = 48000; c.channels = 2; return c;
    }
    std::uint64_t config_revision() const override { return revision; }
    std::string backend_name() const override { return "fake-audio"; }
    opal::PlatformError last_platform_error() const override { return {}; }
    bool running = false;
    std::uint64_t revision = 0;
};
}

int main()
{
    FakeAudio audio;
    assert(audio.start());
    const auto cfg = audio.config();
    assert(cfg.kind == opal::MediaKind::AudioAac);
    assert(cfg.sample_rate == 48000 && cfg.channels == 2 && !cfg.extradata.empty());
    assert(audio.config_revision() == 1);
    opal::EncodedMediaUnit unit;
    assert(audio.next(unit, 0));
    assert(unit.kind == opal::MediaKind::AudioAac && unit.capture_time_us == 55 && !unit.data.empty());
    audio.stop();
    assert(!audio.next(unit, 0));
    return 0;
}
