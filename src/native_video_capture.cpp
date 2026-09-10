#include <opal/video_capture.hpp>

#include <opal/audio_capture_backend.hpp>
#include <opal/capture_backend.hpp>
#include <opal/encoded_buffer_pool.hpp>
#include <opal/host_display.hpp>
#include <opal/native_video_pipeline.hpp>
#include <opal/video_encoder_backend.hpp>
#if defined(_WIN32)
#include <opal/windows_idd_capture.hpp>
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace opal {
namespace {

bool debug_enabled()
{
    const char* value = std::getenv("OPAL_DEBUG");
    return value && *value && std::string(value) != "0";
}

bool same_config(const MediaConfig& a, const MediaConfig& b)
{
    return a.kind == b.kind && a.extradata == b.extradata &&
           a.sample_rate == b.sample_rate && a.channels == b.channels;
}

std::unique_ptr<CaptureBackend> make_target_capture_backend(const DisplayTarget& target)
{
#if defined(_WIN32)
    if (target.capture_kind == DisplayCaptureKind::WindowsIddSwapchain)
        return make_windows_idd_capture_backend();
#else
    (void)target;
#endif
    return make_capture_backend();
}

}

struct VideoCapture::Impl {
    std::unique_ptr<HostDisplayManager> display;
    std::unique_ptr<NativeVideoPipeline> video;
    std::unique_ptr<AudioCaptureBackend> audio;
    EncodedMediaUnit storage;
    std::vector<MediaConfig> configs;
    std::uint64_t config_revision = 0;
    std::uint64_t video_config_revision = 0;
    std::uint64_t audio_config_revision = 0;
    bool running = false;
    bool terminal = false;
    bool audio_requested = false;
    std::string error;

    void recycle_storage()
    {
        if (storage.kind == MediaKind::VideoH264 && !storage.data.empty())
            encoded_buffer_pool().release(std::move(storage.data));
        storage = {};
    }

    void merge_config(const MediaConfig& incoming)
    {
        if (incoming.extradata.empty()) return;
        for (auto& existing : configs) {
            if (existing.kind != incoming.kind) continue;
            if (same_config(existing, incoming)) return;
            existing = incoming;
            ++config_revision;
            return;
        }
        configs.push_back(incoming);
        ++config_revision;
    }

    void sync_configs()
    {
        if (video && video->config_revision() != video_config_revision) {
            video_config_revision = video->config_revision();
            merge_config(video->config());
        }
        if (audio && audio->config_revision() != audio_config_revision) {
            audio_config_revision = audio->config_revision();
            merge_config(audio->config());
        }
    }

    bool make_view(EncodedMediaView& view)
    {
        if (storage.data.empty()) return false;
        view.kind = storage.kind;
        view.data = storage.data;
        view.pts_us = storage.pts_us;
        view.capture_time_us = storage.capture_time_us;
        view.keyframe = storage.keyframe;
        sync_configs();
        return true;
    }

    void capture_error()
    {
        if (video) {
            const auto e = video->last_platform_error();
            if (e) error = e.message;
        }
        if (error.empty() && audio) {
            const auto e = audio->last_platform_error();
            if (e) error = e.message;
        }
        if (error.empty() && display) {
            const auto e = display->last_platform_error();
            if (e) error = e.message;
        }
    }

    void mark_terminal(std::string message = {})
    {
        if (!message.empty()) error = std::move(message);
        if (error.empty()) capture_error();
        terminal = true;
        running = false;
    }

    bool poll(EncodedMediaView& view, int video_wait_ms)
    {
        recycle_storage();

        if (audio) {
            if (audio->next(storage, 0)) return make_view(view);
            const auto audio_error = audio->last_platform_error();
            if (audio_error) {
                mark_terminal(audio_error.message);
                return false;
            }
        }

        recycle_storage();
        if (video && video->next(storage, video_wait_ms)) return make_view(view);
        if (video && video->ended()) mark_terminal();
        if (display && !display->healthy()) mark_terminal("host display became unavailable");
        return false;
    }
};

VideoCapture::VideoCapture() : impl_(std::make_unique<Impl>()) {}
VideoCapture::~VideoCapture() { stop(); }

bool VideoCapture::start(const StreamOptions& stream, int bitrate_kbps, bool audio,
                         const std::string& portal_token_file)
{
    (void)portal_token_file;
    stop();
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->error.clear();
    impl_->terminal = false;
    impl_->audio_requested = audio;
    impl_->configs.clear();
    impl_->config_revision = 0;
    impl_->video_config_revision = 0;
    impl_->audio_config_revision = 0;

    impl_->display = std::make_unique<HostDisplayManager>();
    if (!impl_->display->prepare(stream)) {
        impl_->capture_error();
        if (impl_->error.empty()) impl_->error = "could not prepare a usable host display";
        impl_->display.reset();
        return false;
    }

    const auto& target = impl_->display->target();
    impl_->video = std::make_unique<NativeVideoPipeline>(
        make_target_capture_backend(target), make_video_encoder_backend());
    if (!impl_->video->start(stream, bitrate_kbps, &target)) {
        impl_->capture_error();
        if (impl_->error.empty()) impl_->error = "native video capture failed to start";
        impl_->video.reset();
        impl_->display->stop();
        impl_->display.reset();
        return false;
    }

    if (audio) {
        impl_->audio = make_audio_capture_backend();
        if (!impl_->audio || !impl_->audio->start()) {
            impl_->capture_error();
            if (impl_->error.empty()) impl_->error = "native system audio capture failed to start";
            if (impl_->audio) impl_->audio->stop();
            impl_->audio.reset();
            impl_->video->stop();
            impl_->video.reset();
            impl_->display->stop();
            impl_->display.reset();
            return false;
        }
    }

    impl_->running = true;
    if (debug_enabled()) {
        std::cerr << "OPAL display=" << display_kind_name(target.kind)
                  << " name=" << target.name
                  << " mode=" << target.mode.width << 'x' << target.mode.height << '@' << target.mode.refresh_hz
                  << " provider=" << impl_->display->backend_name() << '\n';
        std::cerr << "OPAL capture backend=" << backend_name()
                  << " acquisition_timestamp=" << capture_timestamp_quality_name(capture_timestamp_quality())
                  << " video_encode=in-process audio=" << (audio ? "native" : "off") << "\n";
    }
    return true;
}

bool VideoCapture::next_view(EncodedMediaView& unit, int timeout_ms)
{
    unit = {};
    if (!impl_ || !impl_->running || !impl_->video) return false;
    impl_->sync_configs();
    if (impl_->poll(unit, 0)) return true;
    if (impl_->terminal || timeout_ms <= 0) return false;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline && impl_->running && !impl_->terminal) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        const int wait_ms = std::max(1, std::min<int>(5, static_cast<int>(remaining)));
        if (impl_->poll(unit, wait_ms)) return true;
    }
    return false;
}

bool VideoCapture::next(EncodedMediaUnit& unit, int timeout_ms)
{
    EncodedMediaView view;
    if (!next_view(view, timeout_ms)) return false;
    unit.kind = view.kind;
    unit.data.assign(view.data.begin(), view.data.end());
    unit.pts_us = view.pts_us;
    unit.capture_time_us = view.capture_time_us;
    unit.keyframe = view.keyframe;
    return !unit.data.empty();
}

bool VideoCapture::request_idr()
{
    if (!impl_ || !impl_->running || impl_->terminal || !impl_->video) return false;
    impl_->video->request_idr();
    return !impl_->video->ended();
}

bool VideoCapture::set_bitrate(int bitrate_kbps)
{
    if (!impl_ || !impl_->running || impl_->terminal || !impl_->video) return false;
    const bool ok = impl_->video->set_bitrate(bitrate_kbps);
    if (!ok && impl_->video->ended()) impl_->mark_terminal();
    return ok;
}

bool VideoCapture::ended() const
{
    return impl_ && impl_->terminal;
}

const std::vector<MediaConfig>& VideoCapture::configs() const
{
    static const std::vector<MediaConfig> empty;
    if (!impl_) return empty;
    impl_->sync_configs();
    return impl_->configs;
}

std::uint64_t VideoCapture::config_revision() const
{
    if (!impl_) return 0;
    impl_->sync_configs();
    return impl_->config_revision;
}

std::string VideoCapture::backend_name() const
{
    if (!impl_ || !impl_->video) return "unavailable";
    auto name = impl_->video->backend_name();
    if (impl_->audio) name += "+" + impl_->audio->backend_name();
    return name;
}

std::string VideoCapture::last_error() const
{
    if (!impl_) return "capture unavailable";
    if (!impl_->error.empty()) return impl_->error;
    impl_->capture_error();
    return impl_->error;
}

CaptureTimestampQuality VideoCapture::capture_timestamp_quality() const
{
    return impl_ && impl_->video ? impl_->video->timestamp_quality() : CaptureTimestampQuality::Estimated;
}

bool VideoCapture::capture_timestamp_estimated() const
{
    return capture_timestamp_quality() == CaptureTimestampQuality::Estimated;
}

void VideoCapture::stop()
{
    if (!impl_) return;
    impl_->running = false;
    if (impl_->audio) impl_->audio->stop();
    if (impl_->video) impl_->video->stop();
    impl_->audio.reset();
    impl_->video.reset();
    if (impl_->display) impl_->display->stop();
    impl_->display.reset();
    impl_->recycle_storage();
    impl_->configs.clear();
    impl_->config_revision = 0;
    impl_->video_config_revision = 0;
    impl_->audio_config_revision = 0;
    impl_->terminal = false;
}

}
