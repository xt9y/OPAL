#import <CoreGraphics/CoreGraphics.h>

#include <opal/video_capture.hpp>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
bool strict_runtime()
{
    const char* value = std::getenv("OPAL_REQUIRE_MACOS_MEDIA");
    return value && *value && std::string(value) != "0";
}
}

int main()
{
    if (!CGPreflightScreenCaptureAccess()) {
        if (strict_runtime()) {
            std::cerr << "macOS native media smoke requires Screen Recording permission\n";
            return 2;
        }
        std::cout << "SKIP macOS native media smoke: Screen Recording permission is not granted\n";
        return 0;
    }

    opal::VideoCapture capture;
    opal::StreamOptions stream{1280, 720, 60};
    if (!capture.start(stream, 12000, true, "")) {
        std::cerr << "macOS native media start failed: " << capture.last_error() << '\n';
        return 1;
    }

    const std::string backend = capture.backend_name();
    assert(backend.find("screencapturekit") != std::string::npos);
    assert(backend.find("videotoolbox-hardware-lowlatency") != std::string::npos);
    assert(backend.find("aac") != std::string::npos);
    assert(capture.set_bitrate(10000));
    assert(capture.request_idr());

    bool video = false;
    bool audio = false;
    bool video_config = false;
    bool audio_config = false;
    bool exact_video_clock = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);

    while (std::chrono::steady_clock::now() < deadline &&
           !(video && audio && video_config && audio_config)) {
        opal::EncodedMediaView unit;
        if (!capture.next_view(unit, 100)) {
            if (capture.ended()) break;
            continue;
        }
        if (unit.kind == opal::MediaKind::VideoH264) {
            video |= !unit.data.empty() && unit.capture_time_us != 0;
            exact_video_clock |= capture.capture_timestamp_quality() == opal::CaptureTimestampQuality::Exact;
        } else if (unit.kind == opal::MediaKind::AudioAac) {
            audio |= !unit.data.empty() && unit.capture_time_us != 0;
        }
        for (const auto &config : capture.configs()) {
            if (config.kind == opal::MediaKind::VideoH264)
                video_config |= !config.extradata.empty();
            else if (config.kind == opal::MediaKind::AudioAac)
                audio_config |= !config.extradata.empty() && config.sample_rate == 48000 && config.channels == 2;
        }
    }

    if (capture.ended()) std::cerr << "macOS native media ended: " << capture.last_error() << '\n';
    capture.stop();

    assert(video);
    assert(audio);
    assert(video_config);
    assert(audio_config);
    assert(exact_video_clock);
    return 0;
}
