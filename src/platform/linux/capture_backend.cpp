#include <opal/capture_backend.hpp>
#include <opal/linux_video_backend.hpp>

#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw-utils.h>

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

namespace opal {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t monotonic_us()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now().time_since_epoch()).count());
}

AVPixelFormat av_format(std::uint32_t format)
{
    switch (format) {
        case SPA_VIDEO_FORMAT_BGRA: return AV_PIX_FMT_BGRA;
        case SPA_VIDEO_FORMAT_BGRx: return AV_PIX_FMT_BGR0;
        case SPA_VIDEO_FORMAT_RGBA: return AV_PIX_FMT_RGBA;
        case SPA_VIDEO_FORMAT_RGBx: return AV_PIX_FMT_RGB0;
        default: return AV_PIX_FMT_NONE;
    }
}

template <class T>
void clear_ptr(T*& value)
{
    value = nullptr;
}

struct CpuFrame {
    std::vector<std::uint8_t> pixels;
    int width = 0;
    int height = 0;
    int stride = 0;
    AVPixelFormat format = AV_PIX_FMT_NONE;
    std::uint64_t capture_us = 0;
};

class LinuxPipeWireCaptureBackend final : public CaptureBackend {
public:
    explicit LinuxPipeWireCaptureBackend(std::uint32_t node_id) : node_id_(node_id) {}
    ~LinuxPipeWireCaptureBackend() override { stop(); }

    bool start(const StreamOptions& stream) override
    {
        stop();
        if (node_id_ == 0 || node_id_ == PW_ID_ANY) {
            set_error(PlatformFailure::Unavailable, "KWin returned an invalid PipeWire node");
            return false;
        }

        fps_ = std::clamp(stream.fps, 15, 240);
        preferred_width_ = stream.max_width > 0 ? std::clamp(stream.max_width, 16, 7680) : 1920;
        preferred_height_ = stream.max_height > 0 ? std::clamp(stream.max_height, 16, 4320) : 1080;

        static std::once_flag pipewire_once;
        std::call_once(pipewire_once, [] { pw_init(nullptr, nullptr); });

        loop_ = pw_thread_loop_new("opal-headless-pipewire", nullptr);
        context_ = loop_ ? pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0) : nullptr;
        core_ = context_ ? pw_context_connect(context_, nullptr, 0) : nullptr;
        if (!loop_ || !context_ || !core_) {
            set_error(PlatformFailure::Unavailable, "could not connect to the user PipeWire daemon");
            stop_resources();
            return false;
        }

        pw_properties* properties = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Video",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Screen",
            PW_KEY_NODE_NAME, "opal.headless.capture",
            nullptr);
        stream_ = pw_stream_new(core_, "OPAL virtual display", properties);
        if (!stream_) {
            set_error(PlatformFailure::Unavailable, "could not create the OPAL PipeWire stream");
            stop_resources();
            return false;
        }

        static const pw_stream_events events = make_events();
        pw_stream_add_listener(stream_, &listener_, &events, this);

        std::array<std::uint8_t, 1024> storage{};
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
        const spa_pod* params[1];
        spa_rectangle preferred_size{static_cast<std::uint32_t>(preferred_width_), static_cast<std::uint32_t>(preferred_height_)};
        spa_rectangle min_size{16, 16};
        spa_rectangle max_size{7680, 4320};
        spa_fraction preferred_rate{static_cast<std::uint32_t>(fps_), 1};
        spa_fraction min_rate{1, 1};
        spa_fraction max_rate{240, 1};
        params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format,
            SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRA,
                                   SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_RGBx),
            SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&preferred_size, &min_size, &max_size),
            SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&preferred_rate, &min_rate, &max_rate)));

        running_ = true;
        const int rc = pw_stream_connect(
            stream_, PW_DIRECTION_INPUT, node_id_,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
            params, 1);
        if (rc < 0 || pw_thread_loop_start(loop_) < 0) {
            set_error(PlatformFailure::OsError, "could not connect to the KWin PipeWire node");
            running_ = false;
            stop_resources();
            return false;
        }
        loop_started_ = true;
        return true;
    }

    bool next(NativeVideoFrame& frame, int timeout_ms) override
    {
        frame = {};
        std::shared_ptr<CpuFrame> latest;
        {
            std::unique_lock<std::mutex> lock(mu_);
            if (serial_ == seen_serial_ && running_ && !failed_) {
                cv_.wait_for(lock, std::chrono::milliseconds(std::max(0, timeout_ms)), [&] {
                    return serial_ != seen_serial_ || failed_ || !running_;
                });
            }
            if (serial_ == seen_serial_ || !latest_) return false;
            seen_serial_ = serial_;
            latest = latest_;
        }

        frame.kind = NativeVideoFrameKind::Cpu;
        frame.width = latest->width;
        frame.height = latest->height;
        frame.stride = latest->stride;
        frame.pixel_format = static_cast<std::uint32_t>(latest->format);
        frame.capture_time_us = latest->capture_us;
        frame.bytes = latest->pixels;
        frame.owner = latest;
        return frame.valid();
    }

    void stop() override
    {
        {
            std::lock_guard<std::mutex> lock(mu_);
            running_ = false;
        }
        cv_.notify_all();
        stop_resources();
        std::lock_guard<std::mutex> lock(mu_);
        latest_.reset();
        raw_info_ = {};
        serial_ = 0;
        seen_serial_ = 0;
        failed_ = false;
        error_ = {};
    }

    CaptureTimestampQuality timestamp_quality() const override
    {
        return CaptureTimestampQuality::Exact;
    }

    std::string backend_name() const override
    {
        return "kwin-pipewire-direct";
    }

    PlatformError last_platform_error() const override
    {
        std::lock_guard<std::mutex> lock(mu_);
        return error_;
    }

private:
    static pw_stream_events make_events()
    {
        pw_stream_events events{};
        events.version = PW_VERSION_STREAM_EVENTS;
        events.state_changed = &LinuxPipeWireCaptureBackend::state_changed;
        events.param_changed = &LinuxPipeWireCaptureBackend::param_changed;
        events.process = &LinuxPipeWireCaptureBackend::process;
        return events;
    }

    static void state_changed(void* data, pw_stream_state, pw_stream_state state, const char* message)
    {
        auto& self = *static_cast<LinuxPipeWireCaptureBackend*>(data);
        if (state != PW_STREAM_STATE_ERROR && state != PW_STREAM_STATE_UNCONNECTED) return;
        std::lock_guard<std::mutex> lock(self.mu_);
        if (!self.running_) return;
        self.failed_ = true;
        self.running_ = false;
        self.error_ = {PlatformComponent::Capture, PlatformFailure::OsError,
                       message && *message ? message : "KWin PipeWire stream disconnected", true};
        self.cv_.notify_all();
    }

    static void param_changed(void* data, std::uint32_t id, const spa_pod* param)
    {
        auto& self = *static_cast<LinuxPipeWireCaptureBackend*>(data);
        if (id != SPA_PARAM_Format || !param || !self.stream_) return;
        spa_video_info_raw info{};
        if (spa_format_video_raw_parse(param, &info) < 0 || av_format(info.format) == AV_PIX_FMT_NONE) return;
        self.raw_info_ = info;

        const int stride = static_cast<int>(info.size.width) * 4;
        const int size = stride * static_cast<int>(info.size.height);
        std::array<std::uint8_t, 768> storage{};
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
        const spa_pod* params[2];
        params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(3, 2, 6),
            SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
            SPA_PARAM_BUFFERS_size, SPA_POD_Int(size),
            SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride),
            SPA_PARAM_BUFFERS_dataType,
            SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_DmaBuf) | (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr))));
        params[1] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
            SPA_PARAM_META_size, SPA_POD_Int(static_cast<int>(sizeof(spa_meta_header)))));
        (void)pw_stream_update_params(self.stream_, params, 2);
    }

    static void process(void* data)
    {
        static_cast<LinuxPipeWireCaptureBackend*>(data)->capture_latest();
    }

    std::uint64_t timestamp_for(pw_buffer* buffer) const
    {
        const auto local_now = monotonic_us();
        if (!stream_ || !buffer) return local_now;
        std::uint64_t source = buffer->time;
        if (buffer->buffer) {
            const auto* header = static_cast<const spa_meta_header*>(
                spa_buffer_find_meta_data(buffer->buffer, SPA_META_Header, sizeof(spa_meta_header)));
            if (header && header->pts != SPA_TIME_INVALID && header->pts >= 0)
                source = static_cast<std::uint64_t>(header->pts);
        }
        if (!source) return local_now;
        const std::uint64_t pipewire_now = pw_stream_get_nsec(stream_);
        if (pipewire_now < source) return local_now;
        const std::uint64_t age_us = (pipewire_now - source) / SPA_NSEC_PER_USEC;
        if (age_us > 5000000u || age_us > local_now) return local_now;
        return local_now - age_us;
    }

    void capture_latest()
    {
        if (!stream_ || !running_) return;
        pw_buffer* selected = nullptr;
        for (;;) {
            pw_buffer* next = pw_stream_dequeue_buffer(stream_);
            if (!next) break;
            if (selected) pw_stream_queue_buffer(stream_, selected);
            selected = next;
        }
        if (!selected) return;
        auto requeue = [&] { pw_stream_queue_buffer(stream_, selected); };

        spa_buffer* buffer = selected->buffer;
        if (!buffer || buffer->n_datas == 0 || raw_info_.size.width == 0 || raw_info_.size.height == 0) {
            requeue();
            return;
        }
        spa_data& data = buffer->datas[0];
        if (!data.chunk || data.chunk->stride <= 0) {
            requeue();
            return;
        }

        const AVPixelFormat format = av_format(raw_info_.format);
        const int width = static_cast<int>(raw_info_.size.width);
        const int height = static_cast<int>(raw_info_.size.height);
        const int source_stride = data.chunk->stride;
        const std::size_t row_bytes = static_cast<std::size_t>(width) * 4u;
        if (format == AV_PIX_FMT_NONE || source_stride < static_cast<int>(row_bytes)) {
            requeue();
            return;
        }

        const std::uint64_t end = static_cast<std::uint64_t>(data.chunk->offset) +
                                  static_cast<std::uint64_t>(height - 1) * static_cast<std::uint64_t>(source_stride) +
                                  row_bytes;
        if (end > data.maxsize) {
            requeue();
            return;
        }

        void* mapping = nullptr;
        const std::uint8_t* base = nullptr;
        if (data.data) base = static_cast<const std::uint8_t*>(data.data);
        else if ((data.type == SPA_DATA_MemFd || data.type == SPA_DATA_DmaBuf) && data.fd >= 0) {
            mapping = mmap(nullptr, data.maxsize, PROT_READ, MAP_PRIVATE, static_cast<int>(data.fd), data.mapoffset);
            if (mapping != MAP_FAILED) base = static_cast<const std::uint8_t*>(mapping);
            else mapping = nullptr;
        }
        if (!base) {
            requeue();
            return;
        }

        auto frame = std::make_shared<CpuFrame>();
        frame->pixels.resize(row_bytes * static_cast<std::size_t>(height));
        frame->width = width;
        frame->height = height;
        frame->stride = static_cast<int>(row_bytes);
        frame->format = format;
        frame->capture_us = timestamp_for(selected);
        const auto* source = base + data.chunk->offset;
        for (int y = 0; y < height; ++y) {
            std::copy_n(source + static_cast<std::ptrdiff_t>(y) * source_stride,
                        row_bytes,
                        frame->pixels.data() + static_cast<std::size_t>(y) * row_bytes);
        }
        if (mapping) munmap(mapping, data.maxsize);
        requeue();

        {
            std::lock_guard<std::mutex> lock(mu_);
            latest_ = std::move(frame);
            ++serial_;
        }
        cv_.notify_one();
    }

    void set_error(PlatformFailure failure, std::string message)
    {
        std::lock_guard<std::mutex> lock(mu_);
        error_ = {PlatformComponent::Capture, failure, std::move(message), true};
    }

    void stop_resources()
    {
        if (loop_ && loop_started_) {
            pw_thread_loop_stop(loop_);
            loop_started_ = false;
        }
        if (stream_) {
            spa_hook_remove(&listener_);
            pw_stream_destroy(stream_);
            clear_ptr(stream_);
        }
        if (core_) {
            pw_core_disconnect(core_);
            clear_ptr(core_);
        }
        if (context_) {
            pw_context_destroy(context_);
            clear_ptr(context_);
        }
        if (loop_) {
            pw_thread_loop_destroy(loop_);
            clear_ptr(loop_);
        }
    }

    std::uint32_t node_id_ = 0;
    int fps_ = 60;
    int preferred_width_ = 1920;
    int preferred_height_ = 1080;

    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_stream* stream_ = nullptr;
    spa_hook listener_{};
    spa_video_info_raw raw_info_{};
    bool loop_started_ = false;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::shared_ptr<CpuFrame> latest_;
    std::uint64_t serial_ = 0;
    std::uint64_t seen_serial_ = 0;
    bool running_ = false;
    bool failed_ = false;
    PlatformError error_{};
};

}

std::unique_ptr<CaptureBackend> make_linux_pipewire_capture_backend(std::uint32_t node_id)
{
    return std::make_unique<LinuxPipeWireCaptureBackend>(node_id);
}

}
