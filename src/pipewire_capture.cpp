#include <opal/pipewire_capture.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef OPAL_HAVE_NATIVE_PIPEWIRE
#define OPAL_HAVE_NATIVE_PIPEWIRE 0
#endif

#if OPAL_HAVE_NATIVE_PIPEWIRE
#include <fcntl.h>
#include <libportal/portal.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw-utils.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}
#endif

namespace opal {
namespace {
using Clock = std::chrono::steady_clock;

std::uint64_t monotonic_us()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}

std::vector<std::uint8_t> annexb_parameter_sets(std::span<const std::uint8_t> data)
{
    std::vector<std::uint8_t> out;
    bool sps = false, pps = false;
    std::size_t pos = 0;
    auto start_code = [&](std::size_t from, std::size_t& begin, std::size_t& bytes) {
        for (std::size_t i = from; i + 3 <= data.size(); ++i) {
            if (data[i] != 0 || data[i + 1] != 0) continue;
            if (data[i + 2] == 1) { begin = i; bytes = 3; return true; }
            if (i + 4 <= data.size() && data[i + 2] == 0 && data[i + 3] == 1) { begin = i; bytes = 4; return true; }
        }
        return false;
    };
    for (;;) {
        std::size_t begin = 0, prefix = 0;
        if (!start_code(pos, begin, prefix)) break;
        const std::size_t nal = begin + prefix;
        if (nal >= data.size()) break;
        std::size_t next = 0, next_prefix = 0;
        const bool have_next = start_code(nal + 1, next, next_prefix);
        const std::size_t end = have_next ? next : data.size();
        const auto type = data[nal] & 0x1f;
        if (type == 7 || type == 8) {
            out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(begin), data.begin() + static_cast<std::ptrdiff_t>(end));
            sps |= type == 7;
            pps |= type == 8;
        }
        if (!have_next) break;
        pos = next;
        (void)next_prefix;
    }
    if (!sps || !pps) out.clear();
    return out;
}

#if OPAL_HAVE_NATIVE_PIPEWIRE

struct HubRawFrame {
    std::vector<std::uint8_t> pixels;
    int width = 0;
    int height = 0;
    int stride = 0;
    AVPixelFormat format = AV_PIX_FMT_BGRA;
    std::uint64_t capture_us = 0;
    bool source_dmabuf = false;
    MonitorGeometry geometry;
};

class PersistentPipeWireHub;

struct MonitorCapture {
    PersistentPipeWireHub* owner = nullptr;
    std::uint32_t node_id = PW_ID_ANY;
    std::uint64_t pipewire_serial = 0;
    pw_stream* stream = nullptr;
    spa_hook listener{};
    spa_video_info_raw raw_info{};
    MonitorGeometry portal_geometry;
    std::mutex frame_mu;
    std::shared_ptr<HubRawFrame> latest;
    std::atomic<bool> active{true};
    SwsContext* compositor_sws = nullptr;

    ~MonitorCapture()
    {
        if (compositor_sws) sws_freeContext(compositor_sws);
    }
};

struct PortalWait {
    GMainLoop* loop = nullptr;
    XdpSession* session = nullptr;
    GError* error = nullptr;
    bool ok = false;
};

void portal_created_cb(GObject* source, GAsyncResult* result, gpointer data)
{
    auto* wait = static_cast<PortalWait*>(data);
    wait->session = xdp_portal_create_screencast_session_finish(XDP_PORTAL(source), result, &wait->error);
    g_main_loop_quit(wait->loop);
}

void portal_started_cb(GObject* source, GAsyncResult* result, gpointer data)
{
    auto* wait = static_cast<PortalWait*>(data);
    wait->ok = xdp_session_start_finish(XDP_SESSION(source), result, &wait->error);
    g_main_loop_quit(wait->loop);
}

AVPixelFormat av_format(std::uint32_t format)
{
    switch (format) {
        case SPA_VIDEO_FORMAT_BGRA: return AV_PIX_FMT_BGRA;
        case SPA_VIDEO_FORMAT_BGRx: return AV_PIX_FMT_BGR0;
        case SPA_VIDEO_FORMAT_RGBA: return AV_PIX_FMT_RGBA;
        case SPA_VIDEO_FORMAT_RGBx: return AV_PIX_FMT_RGB0;
        case SPA_VIDEO_FORMAT_xRGB: return AV_PIX_FMT_0RGB;
        case SPA_VIDEO_FORMAT_xBGR: return AV_PIX_FMT_0BGR;
        case SPA_VIDEO_FORMAT_ARGB: return AV_PIX_FMT_ARGB;
        case SPA_VIDEO_FORMAT_ABGR: return AV_PIX_FMT_ABGR;
        case SPA_VIDEO_FORMAT_xRGB_210LE:
        case SPA_VIDEO_FORMAT_ARGB_210LE:
            return AV_PIX_FMT_X2RGB10LE;
        case SPA_VIDEO_FORMAT_xBGR_210LE:
        case SPA_VIDEO_FORMAT_ABGR_210LE:
            return AV_PIX_FMT_X2BGR10LE;
        default: return AV_PIX_FMT_NONE;
    }
}

class PersistentPipeWireHub {
public:
    ~PersistentPipeWireHub() { shutdown(); }

    bool ensure_started(const StreamOptions& stream, const std::string& token_file)
    {
        std::lock_guard<std::mutex> start_lock(start_mu_);
        if (authorization_lost_.load(std::memory_order_acquire)) return false;
        if (failed_.load(std::memory_order_acquire)) {
            cleanup_started_resources();
            failed_.store(false, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(mu_);
                error_.clear();
                serial_ = 0;
                layout_ = {};
            }
            composite_.clear();
        }
        if (started_.load(std::memory_order_acquire)) return true;
        token_file_ = token_file;
        preferred_width_ = stream.max_width > 0 ? std::clamp(stream.max_width, 16, 7680) : 7680;
        preferred_height_ = stream.max_height > 0 ? std::clamp(stream.max_height, 16, 4320) : 4320;
        fps_ = std::clamp(stream.fps, 15, 240);
        if (!open_portal()) { cleanup_started_resources(); return false; }
        if (!open_pipewire()) { cleanup_started_resources(); return false; }
        started_.store(true, std::memory_order_release);
        return true;
    }

    bool next_composed(HubRawFrame& out, int max_width, int max_height, int timeout_ms, std::uint64_t& seen_serial)
    {
        const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(0, timeout_ms));
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait_until(lock, deadline, [&] { return failed_.load(std::memory_order_acquire) || serial_ != seen_serial; });
                if (failed_.load(std::memory_order_acquire)) return false;
                if (serial_ == seen_serial) return false;
                seen_serial = serial_;
            }

            struct Snapshot { MonitorCapture* monitor = nullptr; std::shared_ptr<HubRawFrame> frame; };
            std::vector<Snapshot> snapshots;
            snapshots.reserve(monitors_.size());
            bool waiting_for_first_frame = false;
            for (const auto& monitor : monitors_) {
                if (!monitor->active.load(std::memory_order_acquire)) continue;
                std::shared_ptr<HubRawFrame> frame;
                { std::lock_guard<std::mutex> lock(monitor->frame_mu); frame = monitor->latest; }
                if (!frame) { waiting_for_first_frame = true; break; }
                snapshots.push_back({monitor.get(), std::move(frame)});
            }
            if (waiting_for_first_frame) {
                if (Clock::now() >= deadline) return false;
                continue;
            }
            if (snapshots.empty()) { set_failure("all selected monitor streams unavailable", false); return false; }

            std::vector<MonitorGeometry> geometry;
            geometry.reserve(snapshots.size());
            for (const auto& snapshot : snapshots) geometry.push_back(snapshot.frame->geometry);
            const auto next_layout = build_composite_layout(geometry, max_width > 0 ? max_width : 7680, max_height > 0 ? max_height : 4320);
            if (!next_layout.valid()) return false;

            const std::size_t bytes = static_cast<std::size_t>(next_layout.canvas_width) * static_cast<std::size_t>(next_layout.canvas_height) * 4u;
            if (composite_.size() != bytes) composite_.assign(bytes, 0);
            else std::fill(composite_.begin(), composite_.end(), 0);

            std::uint64_t oldest_capture = std::numeric_limits<std::uint64_t>::max();
            bool any_dmabuf = false;
            for (std::size_t i = 0; i < snapshots.size(); ++i) {
                auto& snapshot = snapshots[i];
                const auto& frame = *snapshot.frame;
                const auto& tile = next_layout.monitors[i];
                snapshot.monitor->compositor_sws = sws_getCachedContext(snapshot.monitor->compositor_sws,
                    frame.width, frame.height, frame.format, tile.tile_width, tile.tile_height, AV_PIX_FMT_BGRA,
                    SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                if (!snapshot.monitor->compositor_sws) { set_failure("multi-monitor compositor scaler unavailable", false); return false; }
                const std::uint8_t* source[4] = {frame.pixels.data(), nullptr, nullptr, nullptr};
                int source_stride[4] = {frame.stride, 0, 0, 0};
                std::uint8_t* destination[4] = {composite_.data() + static_cast<std::size_t>(tile.tile_x) * 4u, nullptr, nullptr, nullptr};
                int destination_stride[4] = {next_layout.canvas_width * 4, 0, 0, 0};
                if (sws_scale(snapshot.monitor->compositor_sws, source, source_stride, 0, frame.height, destination, destination_stride) <= 0) {
                    set_failure("multi-monitor compositor scaling failed", false); return false;
                }
                oldest_capture = std::min(oldest_capture, frame.capture_us);
                any_dmabuf |= frame.source_dmabuf;
            }

            out.pixels = composite_;
            out.width = next_layout.canvas_width;
            out.height = next_layout.canvas_height;
            out.stride = next_layout.canvas_width * 4;
            out.format = AV_PIX_FMT_BGRA;
            out.capture_us = oldest_capture == std::numeric_limits<std::uint64_t>::max() ? monotonic_us() : oldest_capture;
            out.source_dmabuf = any_dmabuf;
            { std::lock_guard<std::mutex> lock(mu_); layout_ = next_layout; }
            return true;
        }
    }

    CompositeLayout layout() const
    {
        std::lock_guard<std::mutex> lock(mu_);
        return layout_;
    }

    bool authorization_lost() const { return authorization_lost_.load(std::memory_order_acquire); }
    bool failed() const { return failed_.load(std::memory_order_acquire); }

    std::string last_error() const
    {
        std::lock_guard<std::mutex> lock(mu_);
        return error_;
    }

    void capture_latest_buffer(MonitorCapture& monitor)
    {
        if (!monitor.stream || !monitor.active.load(std::memory_order_acquire)) return;
        pw_buffer* selected = nullptr;
        for (;;) {
            pw_buffer* next = pw_stream_dequeue_buffer(monitor.stream);
            if (!next) break;
            if (selected) pw_stream_queue_buffer(monitor.stream, selected);
            selected = next;
        }
        if (!selected) return;
        auto requeue = [&] { pw_stream_queue_buffer(monitor.stream, selected); };
        spa_buffer* buffer = selected->buffer;
        if (!buffer || buffer->n_datas == 0 || monitor.raw_info.size.width == 0 || monitor.raw_info.size.height == 0) { requeue(); return; }
        spa_data& data = buffer->datas[0];
        if (!data.chunk || data.chunk->stride <= 0) { requeue(); return; }
        const AVPixelFormat format = av_format(monitor.raw_info.format);
        if (format == AV_PIX_FMT_NONE) { requeue(); return; }
        const int width = static_cast<int>(monitor.raw_info.size.width);
        const int height = static_cast<int>(monitor.raw_info.size.height);
        const int source_stride = data.chunk->stride;
        const std::size_t row_bytes = static_cast<std::size_t>(width) * 4u;
        if (source_stride < static_cast<int>(row_bytes)) { requeue(); return; }
        const std::uint64_t end = static_cast<std::uint64_t>(data.chunk->offset) + static_cast<std::uint64_t>(height - 1) * static_cast<std::uint64_t>(source_stride) + row_bytes;
        if (end > data.maxsize) { requeue(); return; }

        void* mapping = nullptr;
        const std::uint8_t* base = nullptr;
        if (data.data) base = static_cast<const std::uint8_t*>(data.data);
        else if ((data.type == SPA_DATA_MemFd || data.type == SPA_DATA_DmaBuf) && data.fd >= 0) {
            mapping = mmap(nullptr, data.maxsize, PROT_READ, MAP_PRIVATE, static_cast<int>(data.fd), data.mapoffset);
            if (mapping != MAP_FAILED) base = static_cast<const std::uint8_t*>(mapping);
            else mapping = nullptr;
        }
        if (!base) { requeue(); return; }

        std::shared_ptr<HubRawFrame> frame;
        { std::lock_guard<std::mutex> lock(monitor.frame_mu); if (monitor.latest && monitor.latest.use_count() == 1) frame = std::move(monitor.latest); }
        if (!frame) frame = std::make_shared<HubRawFrame>();
        frame->pixels.resize(row_bytes * static_cast<std::size_t>(height));
        frame->width = width;
        frame->height = height;
        frame->stride = static_cast<int>(row_bytes);
        frame->format = format;
        frame->capture_us = capture_cycle_us(monitor, selected);
        frame->source_dmabuf = data.type == SPA_DATA_DmaBuf;
        frame->geometry = monitor.portal_geometry;
        frame->geometry.source_width = width;
        frame->geometry.source_height = height;
        if (!frame->geometry.logical_geometry_valid) { frame->geometry.logical_width = width; frame->geometry.logical_height = height; }
        const auto* source = base + data.chunk->offset;
        for (int y = 0; y < height; ++y)
            std::copy_n(source + static_cast<std::ptrdiff_t>(y) * source_stride, row_bytes, frame->pixels.data() + static_cast<std::size_t>(y) * row_bytes);
        if (mapping) munmap(mapping, data.maxsize);
        requeue();
        { std::lock_guard<std::mutex> lock(monitor.frame_mu); monitor.latest = std::move(frame); }
        { std::lock_guard<std::mutex> lock(mu_); ++serial_; }
        cv_.notify_all();
    }

    void monitor_failed(MonitorCapture& monitor, const char* message)
    {
        if (!started_.load(std::memory_order_acquire) || !monitor.active.exchange(false, std::memory_order_acq_rel)) return;
        bool any_active = false;
        for (const auto& candidate : monitors_) any_active |= candidate->active.load(std::memory_order_acquire);
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (error_.empty() && message && *message) error_ = message;
            ++serial_;
            if (!any_active) {
                failed_.store(true, std::memory_order_release);
                if (error_.empty()) error_ = "all selected PipeWire monitor streams disconnected";
            }
        }
        cv_.notify_all();
    }

private:
    bool open_portal()
    {
        portal_ = xdp_portal_new();
        if (!portal_) { set_failure("screencast portal unavailable", true); return false; }
        GMainLoop* main_loop = g_main_loop_new(nullptr, false);
        if (!main_loop) { set_failure("GLib main loop unavailable", true); return false; }
        GCancellable* cancel = g_cancellable_new();
        PortalWait wait;
        wait.loop = main_loop;
        const auto restore = read_token();
        xdp_portal_create_screencast_session(portal_, XDP_OUTPUT_MONITOR, XDP_SCREENCAST_FLAG_MULTIPLE,
            XDP_CURSOR_MODE_EMBEDDED, XDP_PERSIST_MODE_PERSISTENT, restore.empty() ? nullptr : restore.c_str(),
            cancel, &portal_created_cb, &wait);
        g_main_loop_run(main_loop);
        if (!wait.session) {
            if (wait.error) { set_failure(wait.error->message, true); g_error_free(wait.error); }
            else set_failure("screen authorization was denied", true);
            g_object_unref(cancel); g_main_loop_unref(main_loop); return false;
        }
        session_ = wait.session;
        wait = {}; wait.loop = main_loop;
        xdp_session_start(session_, nullptr, cancel, &portal_started_cb, &wait);
        g_main_loop_run(main_loop);
        if (!wait.ok) {
            if (wait.error) { set_failure(wait.error->message, true); g_error_free(wait.error); }
            else set_failure("stored screen authorization could not be restored", true);
            g_object_unref(cancel); g_main_loop_unref(main_loop); return false;
        }

        GVariant* streams = xdp_session_get_streams(session_);
        if (!streams) { set_failure("portal returned no PipeWire streams", true); g_object_unref(cancel); g_main_loop_unref(main_loop); return false; }
        const gsize stream_count = g_variant_n_children(streams);
        if (stream_count == 0) {
            g_variant_unref(streams); set_failure("portal returned no PipeWire streams", true);
            g_object_unref(cancel); g_main_loop_unref(main_loop); return false;
        }

        monitors_.clear();
        monitors_.reserve(static_cast<std::size_t>(stream_count));
        for (gsize i = 0; i < stream_count; ++i) {
            guint32 node_id = PW_ID_ANY;
            GVariant* props = nullptr;
            g_variant_get_child(streams, i, "(u@a{sv})", &node_id, &props);
            auto monitor = std::make_unique<MonitorCapture>();
            monitor->owner = this;
            monitor->node_id = static_cast<std::uint32_t>(node_id);
            if (props) {
                gint32 x = 0, y = 0, width = 0, height = 0;
                guint64 pipewire_serial = 0;
                const bool have_position = g_variant_lookup(props, "position", "(ii)", &x, &y);
                const bool have_size = g_variant_lookup(props, "size", "(ii)", &width, &height);
                if (g_variant_lookup(props, "pipewire-serial", "t", &pipewire_serial))
                    monitor->pipewire_serial = static_cast<std::uint64_t>(pipewire_serial);
                if (have_position && have_size && width > 0 && height > 0) {
                    monitor->portal_geometry.logical_x = static_cast<int>(x);
                    monitor->portal_geometry.logical_y = static_cast<int>(y);
                    monitor->portal_geometry.logical_width = static_cast<int>(width);
                    monitor->portal_geometry.logical_height = static_cast<int>(height);
                    monitor->portal_geometry.logical_geometry_valid = true;
                }
                g_variant_unref(props);
            }
            monitors_.push_back(std::move(monitor));
        }
        g_variant_unref(streams);

        remote_fd_ = xdp_session_open_pipewire_remote(session_);
        if (remote_fd_ < 0) { set_failure("portal PipeWire remote unavailable", true); g_object_unref(cancel); g_main_loop_unref(main_loop); return false; }
        if (!save_token_atomic(session_)) {
            set_failure("could not persist rotated screen authorization token", true);
            g_object_unref(cancel); g_main_loop_unref(main_loop); return false;
        }
        g_object_unref(cancel); g_main_loop_unref(main_loop); return true;
    }

    bool open_pipewire()
    {
        pw_init(nullptr, nullptr);
        pipewire_initialized_ = true;
        loop_ = pw_thread_loop_new("opal-pipewire-host", nullptr);
        context_ = loop_ ? pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0) : nullptr;
        core_ = context_ ? pw_context_connect_fd(context_, remote_fd_, nullptr, 0) : nullptr;
        if (core_) remote_fd_ = -1;
        if (!loop_ || !context_ || !core_) { set_failure("PipeWire remote connection failed", false); return false; }

        static const pw_stream_events events = make_stream_events();
        for (std::size_t index = 0; index < monitors_.size(); ++index) {
            auto& monitor = *monitors_[index];
            const std::string name = "OPAL monitor " + std::to_string(index + 1);
            pw_properties* properties = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                                                          PW_KEY_MEDIA_ROLE, "Screen", nullptr);
            if (!properties) { set_failure("PipeWire monitor properties allocation failed", false); return false; }
            if (monitor.pipewire_serial != 0) {
                const std::string serial = std::to_string(monitor.pipewire_serial);
                pw_properties_set(properties, PW_KEY_TARGET_OBJECT, serial.c_str());
            }
            monitor.stream = pw_stream_new(core_, name.c_str(), properties);
            if (!monitor.stream) { set_failure("PipeWire monitor stream allocation failed", false); return false; }
            pw_stream_add_listener(monitor.stream, &monitor.listener, &events, &monitor);

            std::array<std::uint8_t, 1536> pod_buffer{};
            spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer.data(), pod_buffer.size());
            const spa_pod* params[1];
            spa_rectangle default_size{static_cast<std::uint32_t>(preferred_width_), static_cast<std::uint32_t>(preferred_height_)};
            spa_rectangle min_size{16, 16}, max_size{7680, 4320};
            spa_fraction default_rate{static_cast<std::uint32_t>(fps_), 1}, min_rate{1, 1}, max_rate{240, 1};
            params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
                SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(13, SPA_VIDEO_FORMAT_BGRA,
                    SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_RGBx,
                    SPA_VIDEO_FORMAT_xRGB, SPA_VIDEO_FORMAT_xBGR, SPA_VIDEO_FORMAT_ARGB, SPA_VIDEO_FORMAT_ABGR,
                    SPA_VIDEO_FORMAT_xRGB_210LE, SPA_VIDEO_FORMAT_xBGR_210LE,
                    SPA_VIDEO_FORMAT_ARGB_210LE, SPA_VIDEO_FORMAT_ABGR_210LE),
                SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&default_size, &min_size, &max_size),
                SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&default_rate, &min_rate, &max_rate)));
            const std::uint32_t target = monitor.pipewire_serial != 0 ? PW_ID_ANY : monitor.node_id;
            const int rc = pw_stream_connect(monitor.stream, PW_DIRECTION_INPUT, target,
                static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);
            if (rc < 0) { set_failure("PipeWire monitor stream connect failed", false); return false; }
        }
        if (pw_thread_loop_start(loop_) < 0) { set_failure("PipeWire capture loop failed to start", false); return false; }
        loop_started_ = true;
        return true;
    }

    static void stream_state_changed(void* data, pw_stream_state, pw_stream_state state, const char* message)
    {
        auto* monitor = static_cast<MonitorCapture*>(data);
        if (!monitor || !monitor->owner) return;
        if (state != PW_STREAM_STATE_ERROR && state != PW_STREAM_STATE_UNCONNECTED) return;
        monitor->owner->monitor_failed(*monitor, message && *message ? message : "PipeWire monitor stream disconnected");
    }

    static void stream_param_changed(void* data, std::uint32_t id, const spa_pod* param)
    {
        auto* monitor = static_cast<MonitorCapture*>(data);
        if (!monitor || id != SPA_PARAM_Format || !param) return;
        spa_video_info_raw info{};
        if (spa_format_video_raw_parse(param, &info) < 0 || av_format(info.format) == AV_PIX_FMT_NONE) return;
        monitor->raw_info = info;
        if (!monitor->stream || !info.size.width || !info.size.height) return;
        const int stride = static_cast<int>(info.size.width) * 4;
        const int size = stride * static_cast<int>(info.size.height);
        std::array<std::uint8_t, 768> storage{};
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
        const spa_pod* params[2];
        params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8), SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
            SPA_PARAM_BUFFERS_size, SPA_POD_Int(size), SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride), SPA_PARAM_BUFFERS_dataType,
            SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_DmaBuf) | (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr))));
        params[1] = static_cast<const spa_pod*>(spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size, SPA_POD_Int(static_cast<int>(sizeof(spa_meta_header)))));
        (void)pw_stream_update_params(monitor->stream, params, 2);
    }

    static void stream_process(void* data)
    {
        auto* monitor = static_cast<MonitorCapture*>(data);
        if (monitor && monitor->owner) monitor->owner->capture_latest_buffer(*monitor);
    }

    static pw_stream_events make_stream_events()
    {
        pw_stream_events events{};
        events.version = PW_VERSION_STREAM_EVENTS;
        events.state_changed = &PersistentPipeWireHub::stream_state_changed;
        events.param_changed = &PersistentPipeWireHub::stream_param_changed;
        events.process = &PersistentPipeWireHub::stream_process;
        return events;
    }

    std::uint64_t capture_cycle_us(const MonitorCapture& monitor, const pw_buffer* buffer) const
    {
        const auto local_now = monotonic_us();
        if (!monitor.stream || !buffer) return local_now;
        std::uint64_t source_time = buffer->time;
        if (buffer->buffer) {
            const auto* header = static_cast<const spa_meta_header*>(spa_buffer_find_meta_data(buffer->buffer, SPA_META_Header, sizeof(spa_meta_header)));
            if (header && header->pts != SPA_TIME_INVALID && header->pts >= 0) source_time = static_cast<std::uint64_t>(header->pts);
        }
        if (!source_time) return local_now;
        const auto pipewire_now = pw_stream_get_nsec(monitor.stream);
        if (pipewire_now < source_time) return local_now;
        const auto age_us = (pipewire_now - source_time) / SPA_NSEC_PER_USEC;
        if (age_us > 5000000u || age_us > local_now) return local_now;
        return local_now - age_us;
    }

    std::string read_token() const
    {
        std::ifstream in(token_file_);
        std::string token;
        if (in) std::getline(in, token);
        return token;
    }

    bool save_token_atomic(XdpSession* session)
    {
        char* raw = xdp_session_get_restore_token(session);
        if (!raw || !*raw) { g_free(raw); return true; }
        const std::string token(raw);
        g_free(raw);
        if (token_file_.empty()) return true;
        const std::filesystem::path path(token_file_);
        std::error_code ec;
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) return false;
        const std::string temp = path.string() + ".tmp." + std::to_string(static_cast<unsigned long>(getpid()));
        const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0) return false;
        const std::string payload = token + "\n";
        std::size_t offset = 0;
        bool ok = true;
        while (offset < payload.size()) {
            const ssize_t n = ::write(fd, payload.data() + offset, payload.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { ok = false; break; }
            offset += static_cast<std::size_t>(n);
        }
        if (ok && fsync(fd) != 0) ok = false;
        if (::close(fd) != 0) ok = false;
        if (ok && rename(temp.c_str(), path.c_str()) != 0) ok = false;
        if (ok) (void)chmod(path.c_str(), 0600);
        else (void)unlink(temp.c_str());
        return ok;
    }

    void set_failure(std::string text, bool authorization)
    {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (error_.empty()) error_ = std::move(text);
            ++serial_;
        }
        failed_.store(true, std::memory_order_release);
        if (authorization) authorization_lost_.store(true, std::memory_order_release);
        cv_.notify_all();
    }

    void cleanup_started_resources()
    {
        started_.store(false, std::memory_order_release);
        if (loop_ && loop_started_) { pw_thread_loop_stop(loop_); loop_started_ = false; }
        for (auto& monitor : monitors_) {
            if (monitor->stream) { pw_stream_destroy(monitor->stream); monitor->stream = nullptr; }
        }
        monitors_.clear();
        if (core_) { pw_core_disconnect(core_); core_ = nullptr; }
        if (context_) { pw_context_destroy(context_); context_ = nullptr; }
        if (loop_) { pw_thread_loop_destroy(loop_); loop_ = nullptr; }
        if (remote_fd_ >= 0) { close(remote_fd_); remote_fd_ = -1; }
        if (session_) { xdp_session_close(session_); g_object_unref(session_); session_ = nullptr; }
        if (portal_) { g_object_unref(portal_); portal_ = nullptr; }
        if (pipewire_initialized_) { pw_deinit(); pipewire_initialized_ = false; }
    }

    void shutdown()
    {
        std::lock_guard<std::mutex> start_lock(start_mu_);
        cleanup_started_resources();
    }

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::mutex start_mu_;
    std::atomic<bool> started_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> authorization_lost_{false};
    bool pipewire_initialized_ = false;
    bool loop_started_ = false;
    std::string error_;
    std::string token_file_;
    XdpPortal* portal_ = nullptr;
    XdpSession* session_ = nullptr;
    int remote_fd_ = -1;
    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    std::vector<std::unique_ptr<MonitorCapture>> monitors_;
    std::uint64_t serial_ = 0;
    CompositeLayout layout_;
    std::vector<std::uint8_t> composite_;
    int preferred_width_ = 7680;
    int preferred_height_ = 4320;
    int fps_ = 60;
};

PersistentPipeWireHub& pipewire_hub()
{
    static PersistentPipeWireHub hub;
    return hub;
}

#endif
}

struct NativePipeWireVideoCapture::Impl {
    mutable std::mutex mu;
    std::condition_variable encoded_cv;
    std::deque<EncodedMediaUnit> encoded;
    MediaConfig config;
    std::uint64_t config_rev = 0;
    std::string backend = "unavailable", error;
    std::atomic<bool> run{false}, terminal{false};
    std::thread setup_thread;

#if OPAL_HAVE_NATIVE_PIPEWIRE
    struct EncoderState {
        AVCodecContext* ctx = nullptr;
        AVFrame* sw_frame = nullptr;
        AVFrame* hw_frame = nullptr;
        AVPacket* packet = nullptr;
        SwsContext* sws = nullptr;
        AVBufferRef* hw_device = nullptr;
        bool hardware = false;
        AVPixelFormat convert_format = AV_PIX_FMT_YUV420P;
        std::string name;
        void reset()
        {
            if (sws) { sws_freeContext(sws); sws = nullptr; }
            if (packet) av_packet_free(&packet);
            if (sw_frame) av_frame_free(&sw_frame);
            if (hw_frame) av_frame_free(&hw_frame);
            if (ctx) avcodec_free_context(&ctx);
            av_buffer_unref(&hw_device);
            hardware = false;
            convert_format = AV_PIX_FMT_YUV420P;
            name.clear();
        }
        ~EncoderState() { reset(); }
    };

    std::atomic<int> bitrate_kbps{0};
    std::atomic<std::uint64_t> bitrate_generation{0};
    int fps = 60;
    int preferred_width = 7680;
    int preferred_height = 4320;
    std::string token_file;
    std::uint64_t seen_hub_serial = 0;
    std::atomic<bool> saw_dmabuf{false};

    void set_error(std::string text)
    {
        std::lock_guard<std::mutex> lock(mu);
        if (error.empty()) error = std::move(text);
        terminal.store(true);
        encoded_cv.notify_all();
    }

    void configure_common(AVCodecContext* ctx, const HubRawFrame& raw, AVPixelFormat format)
    {
        ctx->width = raw.width;
        ctx->height = raw.height;
        ctx->pix_fmt = format;
        ctx->time_base = AVRational{1, std::max(15, fps)};
        ctx->framerate = AVRational{std::max(15, fps), 1};
        ctx->bit_rate = static_cast<std::int64_t>(std::max(1000, bitrate_kbps.load(std::memory_order_acquire))) * 1000;
        ctx->gop_size = normal_gop_frames(fps);
        ctx->max_b_frames = 0;
        ctx->thread_count = 1;
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY | AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    bool allocate_sw_frame(EncoderState& enc, const HubRawFrame& raw)
    {
        enc.sw_frame = av_frame_alloc();
        enc.packet = av_packet_alloc();
        if (!enc.sw_frame || !enc.packet) return false;
        enc.sw_frame->format = enc.convert_format;
        enc.sw_frame->width = raw.width;
        enc.sw_frame->height = raw.height;
        if (av_frame_get_buffer(enc.sw_frame, 32) < 0) return false;
        enc.sws = sws_getContext(raw.width, raw.height, raw.format, raw.width, raw.height, enc.convert_format,
                                 SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        return enc.sws != nullptr;
    }

    bool open_vaapi(EncoderState& enc, const HubRawFrame& raw)
    {
        const AVCodec* codec = avcodec_find_encoder_by_name("h264_vaapi");
        if (!codec) return false;
        if (av_hwdevice_ctx_create(&enc.hw_device, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) < 0 || !enc.hw_device) { enc.reset(); return false; }
        enc.ctx = avcodec_alloc_context3(codec);
        if (!enc.ctx) { enc.reset(); return false; }
        enc.convert_format = AV_PIX_FMT_NV12;
        configure_common(enc.ctx, raw, AV_PIX_FMT_VAAPI);
        enc.ctx->slices = low_latency_h264_slices(raw.width, raw.height);
        AVBufferRef* frames_ref = av_hwframe_ctx_alloc(enc.hw_device);
        if (!frames_ref) { enc.reset(); return false; }
        auto* frames = reinterpret_cast<AVHWFramesContext*>(frames_ref->data);
        frames->format = AV_PIX_FMT_VAAPI;
        frames->sw_format = AV_PIX_FMT_NV12;
        frames->width = raw.width;
        frames->height = raw.height;
        frames->initial_pool_size = 4;
        if (av_hwframe_ctx_init(frames_ref) < 0) { av_buffer_unref(&frames_ref); enc.reset(); return false; }
        enc.ctx->hw_frames_ctx = av_buffer_ref(frames_ref);
        av_buffer_unref(&frames_ref);
        if (!enc.ctx->hw_frames_ctx) { enc.reset(); return false; }
        AVDictionary* options = nullptr;
        av_dict_set(&options, "async_depth", "1", 0);
        const int rc = avcodec_open2(enc.ctx, codec, &options);
        av_dict_free(&options);
        if (rc < 0) { enc.reset(); return false; }
        enc.hw_frame = av_frame_alloc();
        if (!enc.hw_frame || !allocate_sw_frame(enc, raw)) { enc.reset(); return false; }
        enc.hardware = true;
        enc.name = "h264_vaapi";
        return true;
    }

    static bool accepts_cpu_format(const AVCodec* codec, AVPixelFormat& format)
    {
        if (!codec) return false;
        const AVPixelFormat* formats = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61,12,100)
        int count = 0;
        if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                         reinterpret_cast<const void**>(&formats), &count) < 0) return false;
        if (!formats) { format = AV_PIX_FMT_YUV420P; return true; }
        for (int i = 0; i < count; ++i) if (formats[i] == AV_PIX_FMT_NV12) { format = AV_PIX_FMT_NV12; return true; }
        for (int i = 0; i < count; ++i) if (formats[i] == AV_PIX_FMT_YUV420P) { format = AV_PIX_FMT_YUV420P; return true; }
#else
        formats = codec->pix_fmts;
        if (!formats) { format = AV_PIX_FMT_YUV420P; return true; }
        for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) if (*p == AV_PIX_FMT_NV12) { format = *p; return true; }
        for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) if (*p == AV_PIX_FMT_YUV420P) { format = *p; return true; }
#endif
        return false;
    }

    bool open_cpu_input_encoder(EncoderState& enc, const HubRawFrame& raw)
    {
        constexpr const char* candidates[] = {"h264_v4l2m2m", "h264_nvenc", "h264_qsv", "libx264", "libopenh264"};
        for (const char* name : candidates) {
            const AVCodec* codec = avcodec_find_encoder_by_name(name);
            AVPixelFormat format = AV_PIX_FMT_NONE;
            if (!accepts_cpu_format(codec, format)) continue;
            enc.ctx = avcodec_alloc_context3(codec);
            if (!enc.ctx) continue;
            enc.convert_format = format;
            configure_common(enc.ctx, raw, format);
            AVDictionary* options = nullptr;
            const std::string encoder_name = name;
            if (encoder_name == "h264_nvenc") {
                av_dict_set(&options, "preset", "p1", 0); av_dict_set(&options, "tune", "ull", 0);
                av_dict_set(&options, "zerolatency", "1", 0); av_dict_set(&options, "delay", "0", 0);
            } else if (encoder_name == "h264_qsv") {
                av_dict_set(&options, "async_depth", "1", 0); av_dict_set(&options, "preset", "veryfast", 0);
            } else if (encoder_name == "libx264") {
                enc.ctx->slices = low_latency_h264_slices(raw.width, raw.height);
                av_dict_set(&options, "preset", "ultrafast", 0); av_dict_set(&options, "tune", "zerolatency", 0);
            }
            const int rc = avcodec_open2(enc.ctx, codec, &options);
            av_dict_free(&options);
            if (rc < 0) { enc.reset(); continue; }
            if (!allocate_sw_frame(enc, raw)) { enc.reset(); continue; }
            enc.hardware = encoder_name != "libx264" && encoder_name != "libopenh264";
            enc.name = name;
            return true;
        }
        return false;
    }

    void publish_config(AVCodecContext* ctx, std::span<const std::uint8_t> packet, bool keyframe)
    {
        if (!config.extradata.empty()) return;
        std::vector<std::uint8_t> extra;
        if (ctx && ctx->extradata && ctx->extradata_size > 0) extra.assign(ctx->extradata, ctx->extradata + ctx->extradata_size);
        else if (keyframe) extra = annexb_parameter_sets(packet);
        if (extra.empty()) return;
        config.kind = MediaKind::VideoH264;
        config.extradata = std::move(extra);
        ++config_rev;
    }

    bool configure_encoder(EncoderState& enc, const HubRawFrame& raw)
    {
        enc.reset();
        if (!open_vaapi(enc, raw) && !open_cpu_input_encoder(enc, raw)) { set_error("no usable in-process low-latency H.264 encoder"); return false; }
        std::lock_guard<std::mutex> lock(mu);
        backend = std::string("pipewire-persistent+multimonitor+") + enc.name + (enc.hardware ? "+hw" : "+sw") + (saw_dmabuf.load() ? "+dmabuf-source" : "");
        publish_config(enc.ctx, {}, false);
        return true;
    }

    bool encode_one(EncoderState& enc, const HubRawFrame& raw, std::uint64_t frame_index)
    {
        if (av_frame_make_writable(enc.sw_frame) < 0) return false;
        const std::uint8_t* source[4] = {raw.pixels.data(), nullptr, nullptr, nullptr};
        int source_stride[4] = {raw.stride, 0, 0, 0};
        if (sws_scale(enc.sws, source, source_stride, 0, raw.height, enc.sw_frame->data, enc.sw_frame->linesize) <= 0) return false;
        enc.sw_frame->pts = static_cast<std::int64_t>(frame_index);
        AVFrame* submit = enc.sw_frame;
        if (enc.ctx->pix_fmt == AV_PIX_FMT_VAAPI) {
            av_frame_unref(enc.hw_frame);
            if (av_hwframe_get_buffer(enc.ctx->hw_frames_ctx, enc.hw_frame, 0) < 0 || av_hwframe_transfer_data(enc.hw_frame, enc.sw_frame, 0) < 0) return false;
            enc.hw_frame->pts = enc.sw_frame->pts;
            submit = enc.hw_frame;
        }
        if (avcodec_send_frame(enc.ctx, submit) < 0) return false;
        for (;;) {
            const int rc = avcodec_receive_packet(enc.ctx, enc.packet);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            if (rc < 0) return false;
            const bool keyframe = (enc.packet->flags & AV_PKT_FLAG_KEY) != 0;
            { std::lock_guard<std::mutex> lock(mu); publish_config(enc.ctx, std::span<const std::uint8_t>(enc.packet->data, enc.packet->size), keyframe); }
            EncodedMediaUnit unit;
            unit.kind = MediaKind::VideoH264;
            unit.data.assign(enc.packet->data, enc.packet->data + enc.packet->size);
            unit.pts_us = static_cast<std::int64_t>(raw.capture_us);
            unit.capture_time_us = raw.capture_us;
            unit.keyframe = keyframe;
            av_packet_unref(enc.packet);
            { std::lock_guard<std::mutex> lock(mu); if (encoded.size() >= 2) encoded.pop_front(); encoded.push_back(std::move(unit)); }
            encoded_cv.notify_one();
        }
        return true;
    }

    void encoder_loop()
    {
        EncoderState enc;
        std::uint64_t frame_index = 0;
        std::uint64_t applied_bitrate_generation = bitrate_generation.load(std::memory_order_acquire);
        int width = 0, height = 0;
        AVPixelFormat input = AV_PIX_FMT_NONE;
        while (run.load()) {
            HubRawFrame raw;
            if (!pipewire_hub().next_composed(raw, preferred_width, preferred_height, 20, seen_hub_serial)) {
                if (pipewire_hub().authorization_lost()) {
                    auto reason = pipewire_hub().last_error();
                    if (reason.empty()) reason = "Linux screen authorization lost; rerun OPAL host screen authorization";
                    set_error(reason); break;
                }
                if (pipewire_hub().failed()) {
                    auto reason = pipewire_hub().last_error();
                    if (reason.empty()) reason = "Linux PipeWire capture failed";
                    set_error(reason); break;
                }
                continue;
            }
            if (raw.source_dmabuf) saw_dmabuf.store(true);
            const auto requested_bitrate_generation = bitrate_generation.load(std::memory_order_acquire);
            const bool bitrate_changed = enc.ctx && requested_bitrate_generation != applied_bitrate_generation;
            if (!enc.ctx || raw.width != width || raw.height != height || raw.format != input || bitrate_changed) {
                { std::lock_guard<std::mutex> lock(mu); config = {}; encoded.clear(); ++config_rev; }
                if (!configure_encoder(enc, raw)) break;
                width = raw.width; height = raw.height; input = raw.format; frame_index = 0;
                applied_bitrate_generation = requested_bitrate_generation;
            }
            if (!encode_one(enc, raw, frame_index++)) { set_error("native H.264 encode failed"); break; }
        }
    }

    void setup_loop()
    {
        StreamOptions requested{preferred_width, preferred_height, fps};
        if (!pipewire_hub().ensure_started(requested, token_file)) {
            auto reason = pipewire_hub().last_error();
            if (reason.empty()) reason = "persistent PipeWire capture unavailable";
            set_error(reason); return;
        }
        { std::lock_guard<std::mutex> lock(mu); backend = "pipewire-persistent+multimonitor+starting"; }
        encoder_loop();
    }
#endif
};

bool native_pipewire_prepare(const StreamOptions& stream, const std::string& restore_token_file, std::string* error)
{
#if OPAL_HAVE_NATIVE_PIPEWIRE
    const bool ok = pipewire_hub().ensure_started(stream, restore_token_file);
    if (!ok && error) *error = pipewire_hub().last_error();
    return ok;
#else
    (void)stream; (void)restore_token_file;
    if (error) *error = "native PipeWire capture not compiled";
    return false;
#endif
}

CompositeLayout native_pipewire_layout()
{
#if OPAL_HAVE_NATIVE_PIPEWIRE
    return pipewire_hub().layout();
#else
    return {};
#endif
}

bool native_pipewire_authorization_lost()
{
#if OPAL_HAVE_NATIVE_PIPEWIRE
    return pipewire_hub().authorization_lost();
#else
    return false;
#endif
}

std::string native_pipewire_last_error()
{
#if OPAL_HAVE_NATIVE_PIPEWIRE
    return pipewire_hub().last_error();
#else
    return "native PipeWire capture not compiled";
#endif
}

NativePipeWireVideoCapture::NativePipeWireVideoCapture() : impl_(std::make_unique<Impl>()) {}
NativePipeWireVideoCapture::~NativePipeWireVideoCapture() { stop(); }
bool NativePipeWireVideoCapture::compiled() { return OPAL_HAVE_NATIVE_PIPEWIRE != 0; }

bool NativePipeWireVideoCapture::start(const StreamOptions& stream, int bitrate_kbps, const std::string& restore_token_file)
{
    stop();
    impl_ = std::make_unique<Impl>();
#if OPAL_HAVE_NATIVE_PIPEWIRE
    impl_->bitrate_kbps.store(std::max(1000, bitrate_kbps), std::memory_order_release);
    impl_->bitrate_generation.store(1, std::memory_order_release);
    impl_->fps = std::clamp(stream.fps, 15, 240);
    impl_->preferred_width = stream.max_width > 0 ? std::clamp(stream.max_width, 16, 7680) : 7680;
    impl_->preferred_height = stream.max_height > 0 ? std::clamp(stream.max_height, 16, 4320) : 4320;
    impl_->token_file = restore_token_file;
    impl_->run.store(true);
    impl_->terminal.store(false);
    impl_->backend = "pipewire-persistent+multimonitor+initializing";
    impl_->setup_thread = std::thread([this] { impl_->setup_loop(); });
    return true;
#else
    (void)stream; (void)bitrate_kbps; (void)restore_token_file;
    impl_->error = "native PipeWire capture not compiled";
    impl_->terminal.store(true);
    return false;
#endif
}

bool NativePipeWireVideoCapture::next(EncodedMediaUnit& unit, int timeout_ms)
{
    if (!impl_) return false;
    std::unique_lock<std::mutex> lock(impl_->mu);
    impl_->encoded_cv.wait_for(lock, std::chrono::milliseconds(std::max(0, timeout_ms)), [&] {
        return !impl_->encoded.empty() || impl_->terminal.load() || !impl_->run.load();
    });
    if (impl_->encoded.empty()) return false;
    unit = std::move(impl_->encoded.back());
    impl_->encoded.clear();
    return !unit.data.empty();
}

bool NativePipeWireVideoCapture::set_bitrate(int bitrate_kbps)
{
#if OPAL_HAVE_NATIVE_PIPEWIRE
    if (!impl_ || !impl_->run.load(std::memory_order_acquire) || impl_->terminal.load(std::memory_order_acquire)) return false;
    const int next = std::max(1000, bitrate_kbps);
    const int previous = impl_->bitrate_kbps.exchange(next, std::memory_order_acq_rel);
    if (previous != next) impl_->bitrate_generation.fetch_add(1, std::memory_order_acq_rel);
    return true;
#else
    (void)bitrate_kbps;
    return false;
#endif
}

bool NativePipeWireVideoCapture::ended() const { return !impl_ || impl_->terminal.load(); }
std::uint64_t NativePipeWireVideoCapture::config_revision() const { if (!impl_) return 0; std::lock_guard<std::mutex> lock(impl_->mu); return impl_->config_rev; }
MediaConfig NativePipeWireVideoCapture::config() const { if (!impl_) return {}; std::lock_guard<std::mutex> lock(impl_->mu); return impl_->config; }
std::string NativePipeWireVideoCapture::backend_name() const { if (!impl_) return "unavailable"; std::lock_guard<std::mutex> lock(impl_->mu); return impl_->backend; }
std::string NativePipeWireVideoCapture::last_error() const { if (!impl_) return "native capture unavailable"; std::lock_guard<std::mutex> lock(impl_->mu); return impl_->error; }

void NativePipeWireVideoCapture::stop()
{
    if (!impl_) return;
    impl_->run.store(false);
    impl_->encoded_cv.notify_all();
#if OPAL_HAVE_NATIVE_PIPEWIRE
    if (impl_->setup_thread.joinable()) impl_->setup_thread.join();
#endif
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->encoded.clear();
    impl_->config = {};
    impl_->config_rev = 0;
    impl_->terminal.store(false);
    impl_->backend = "unavailable";
    impl_->error.clear();
}

}