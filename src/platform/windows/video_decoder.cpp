#include <opal/video_decoder.hpp>
#include <opal/encoded_buffer_pool.hpp>

#include <climits>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
}

namespace opal {
namespace {
constexpr std::size_t kOwnedPacketPaddingReserve = 64;
static_assert(AV_INPUT_BUFFER_PADDING_SIZE <= kOwnedPacketPaddingReserve);

void release_owned_packet(void* opaque, std::uint8_t*)
{
    auto* holder = static_cast<std::shared_ptr<std::vector<std::uint8_t>>*>(opaque);
    auto storage = std::move(*holder);
    delete holder;
    if (storage && storage.use_count() == 1) encoded_buffer_pool().release(std::move(*storage));
}
}

struct VideoDecoder::Impl {
    AVCodecContext* ctx = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* scratch = nullptr;
    AVFrame* latest = nullptr;
    std::vector<std::uint8_t> packet_bytes;
    std::vector<std::uint8_t> configured_extradata;
    const AVCodec* codec = nullptr;
    std::string backend = "unconfigured";
    AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;
    std::int64_t latest_pts_us = 0;
    bool active_auto_hardware = false;
    bool auto_hardware_disabled = false;

    bool ensure_io()
    {
        if (!packet) packet = av_packet_alloc();
        if (!scratch) scratch = av_frame_alloc();
        if (!latest) latest = av_frame_alloc();
        return packet && scratch && latest;
    }

    void clear_latest()
    {
        if (latest) av_frame_unref(latest);
        latest_pts_us = 0;
    }

    static AVPixelFormat choose_hw_format(AVCodecContext* context, const AVPixelFormat* formats)
    {
        auto* self = static_cast<Impl*>(context ? context->opaque : nullptr);
        if (!self || !formats) return AV_PIX_FMT_NONE;
        for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format)
            if (*format == self->hw_pix_fmt) return *format;
        return AV_PIX_FMT_NONE;
    }

    bool copy_extradata(std::span<const std::uint8_t> extradata)
    {
        if (!ctx || extradata.empty()) return ctx != nullptr;
        ctx->extradata = static_cast<std::uint8_t*>(av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!ctx->extradata) return false;
        std::memcpy(ctx->extradata, extradata.data(), extradata.size());
        ctx->extradata_size = static_cast<int>(extradata.size());
        return true;
    }

    void configure_common()
    {
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        ctx->flags2 |= AV_CODEC_FLAG2_FAST;
        ctx->pkt_timebase = AVRational{1, 1000000};
        ctx->time_base = AVRational{1, 1000000};
    }

    void reset_context()
    {
        if (ctx) avcodec_free_context(&ctx);
        hw_pix_fmt = AV_PIX_FMT_NONE;
        active_auto_hardware = false;
    }

    bool open_software(const AVCodec* decoder, std::span<const std::uint8_t> extradata)
    {
        reset_context();
        ctx = avcodec_alloc_context3(decoder);
        if (!ctx) return false;
        configure_common();
        ctx->thread_count = 2;
        ctx->thread_type = FF_THREAD_SLICE;
        if (!copy_extradata(extradata) || avcodec_open2(ctx, decoder, nullptr) < 0) {
            reset_context();
            return false;
        }
        backend = (ctx->active_thread_type & FF_THREAD_SLICE) ? "software-slice2" : "software-lowdelay";
        return true;
    }

    bool open_d3d11(const AVCodec* decoder, const AVCodecHWConfig* config,
                    std::span<const std::uint8_t> extradata)
    {
        if (!decoder || !config || config->device_type != AV_HWDEVICE_TYPE_D3D11VA ||
            config->pix_fmt != AV_PIX_FMT_D3D11 ||
            !(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) return false;

        AVBufferRef* device = nullptr;
        if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) < 0 || !device) {
            av_buffer_unref(&device);
            return false;
        }

        reset_context();
        ctx = avcodec_alloc_context3(decoder);
        if (!ctx) {
            av_buffer_unref(&device);
            return false;
        }
        configure_common();
        hw_pix_fmt = AV_PIX_FMT_D3D11;
        ctx->opaque = this;
        ctx->get_format = &Impl::choose_hw_format;
        ctx->thread_count = 1;
        ctx->thread_type = 0;
        ctx->hw_device_ctx = av_buffer_ref(device);
        av_buffer_unref(&device);
        if (!ctx->hw_device_ctx || !copy_extradata(extradata) || avcodec_open2(ctx, decoder, nullptr) < 0) {
            reset_context();
            return false;
        }
        backend = "hardware-d3d11va+direct";
        return true;
    }

    bool fallback_to_software()
    {
        if (!active_auto_hardware || !codec) return false;
        auto_hardware_disabled = true;
        clear_latest();
        if (scratch) av_frame_unref(scratch);
        if (packet) av_packet_unref(packet);
        return open_software(codec, configured_extradata);
    }

    bool drain(std::int64_t pts_us, DecodedVideoView& out, std::size_t& superseded)
    {
        out = {};
        superseded = 0;
        std::size_t decoded = 0;
        for (;;) {
            const int rc = avcodec_receive_frame(ctx, scratch);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            if (rc < 0) return false;
            if ((scratch->flags & AV_FRAME_FLAG_CORRUPT) != 0 || scratch->decode_error_flags != 0) {
                av_frame_unref(scratch);
                clear_latest();
                return false;
            }

            // D3D11VA AVFrames own the texture reference through their AVBufferRef.
            // Move the frame directly into latest_ so the presenter can consume
            // frame->data[0]/data[1] without an av_hwframe_transfer_data readback.
            av_frame_unref(latest);
            av_frame_move_ref(latest, scratch);
            latest_pts_us = pts_us;
            ++decoded;
        }
        if (decoded == 0) return true;
        superseded = decoded - 1;
        out.frame = latest;
        out.pts_us = latest_pts_us;
        return true;
    }

    bool submit_borrowed(std::span<const std::uint8_t> unit, std::int64_t pts_us,
                         DecodedVideoView& out, std::size_t& superseded)
    {
        const std::size_t needed = unit.size() + AV_INPUT_BUFFER_PADDING_SIZE;
        if (packet_bytes.size() < needed) packet_bytes.resize(needed);
        std::memcpy(packet_bytes.data(), unit.data(), unit.size());
        std::memset(packet_bytes.data() + unit.size(), 0, AV_INPUT_BUFFER_PADDING_SIZE);
        av_packet_unref(packet);
        packet->data = packet_bytes.data();
        packet->size = static_cast<int>(unit.size());
        packet->pts = pts_us;
        packet->dts = pts_us;
        const int send_rc = avcodec_send_packet(ctx, packet);
        packet->data = nullptr;
        packet->size = 0;
        if (send_rc < 0) return false;
        return drain(pts_us, out, superseded);
    }

    bool submit_owned(const std::shared_ptr<std::vector<std::uint8_t>>& storage,
                      std::size_t payload_size, std::int64_t pts_us,
                      DecodedVideoView& out, std::size_t& superseded)
    {
        if (!storage || storage->empty() || payload_size == 0 ||
            payload_size > static_cast<std::size_t>(INT_MAX) ||
            storage->size() < payload_size + AV_INPUT_BUFFER_PADDING_SIZE) return false;

        av_packet_unref(packet);
        auto* holder = new (std::nothrow) std::shared_ptr<std::vector<std::uint8_t>>(storage);
        if (!holder) return false;
        AVBufferRef* buffer = av_buffer_create(storage->data(), storage->size(), release_owned_packet,
                                               holder, AV_BUFFER_FLAG_READONLY);
        if (!buffer) {
            delete holder;
            return false;
        }
        packet->buf = buffer;
        packet->data = storage->data();
        packet->size = static_cast<int>(payload_size);
        packet->pts = pts_us;
        packet->dts = pts_us;
        const int send_rc = avcodec_send_packet(ctx, packet);
        av_packet_unref(packet);
        if (send_rc < 0) return false;
        return drain(pts_us, out, superseded);
    }
};

VideoDecoder::VideoDecoder() : impl_(new Impl) {}

bool VideoDecoder::configure_h264(std::span<const std::uint8_t> extradata)
{
    if (!impl_) return false;
    impl_->reset_context();
    impl_->clear_latest();
    impl_->backend = "unconfigured";
    if (impl_->scratch) av_frame_unref(impl_->scratch);

    const std::string requested = [] {
        const char* value = std::getenv("OPAL_DECODER");
        return value && *value ? std::string(value) : std::string("auto");
    }();

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec || !impl_->ensure_io()) return false;
    impl_->codec = codec;
    impl_->configured_extradata.assign(extradata.begin(), extradata.end());
    const std::span<const std::uint8_t> saved(impl_->configured_extradata.data(), impl_->configured_extradata.size());

    if (requested == "software" || (requested == "auto" && impl_->auto_hardware_disabled))
        return impl_->open_software(codec, saved);

    const bool request_d3d11 = requested == "auto" || requested == "d3d11va" || requested == "d3d11";
    if (request_d3d11) {
        for (int index = 0;; ++index) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(codec, index);
            if (!config) break;
            if (config->device_type != AV_HWDEVICE_TYPE_D3D11VA || config->pix_fmt != AV_PIX_FMT_D3D11) continue;
            if (impl_->open_d3d11(codec, config, saved)) {
                impl_->active_auto_hardware = requested == "auto";
                return true;
            }
        }
    }

    if (requested != "auto") return false;
    impl_->auto_hardware_disabled = true;
    return impl_->open_software(codec, saved);
}

bool VideoDecoder::decode_latest(std::span<const std::uint8_t> unit, std::int64_t pts_us,
                                 DecodedVideoView& out, std::size_t& superseded)
{
    if (!impl_ || !impl_->ctx || unit.empty() || unit.size() > static_cast<std::size_t>(INT_MAX) || !impl_->ensure_io()) {
        out = {};
        superseded = 0;
        return false;
    }
    if (impl_->submit_borrowed(unit, pts_us, out, superseded)) return true;
    if (!impl_->fallback_to_software()) return false;
    return impl_->submit_borrowed(unit, pts_us, out, superseded);
}

bool VideoDecoder::decode_latest_owned(std::vector<std::uint8_t> unit, std::int64_t pts_us,
                                       DecodedVideoView& out, std::size_t& superseded)
{
    if (!impl_ || !impl_->ctx || unit.empty() || unit.size() > static_cast<std::size_t>(INT_MAX) || !impl_->ensure_io()) {
        out = {};
        superseded = 0;
        return false;
    }
    const std::size_t payload_size = unit.size();
    if (unit.capacity() < payload_size + kOwnedPacketPaddingReserve)
        unit.reserve(payload_size + kOwnedPacketPaddingReserve);
    unit.resize(payload_size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    auto storage = std::make_shared<std::vector<std::uint8_t>>(std::move(unit));
    if (impl_->submit_owned(storage, payload_size, pts_us, out, superseded)) return true;
    if (!impl_->fallback_to_software()) return false;
    return impl_->submit_owned(storage, payload_size, pts_us, out, superseded);
}

bool VideoDecoder::take_latest(DecodedVideoFrame& out)
{
    if (!impl_ || !impl_->latest || impl_->latest->width <= 0 || impl_->latest->height <= 0) return false;
    AVFrame* replacement = av_frame_alloc();
    if (!replacement) return false;
    if (out.frame) av_frame_free(&out.frame);
    out.frame = impl_->latest;
    out.pts_us = impl_->latest_pts_us;
    impl_->latest = replacement;
    impl_->latest_pts_us = 0;
    return true;
}

bool VideoDecoder::decode(std::span<const std::uint8_t> unit, std::int64_t pts_us,
                          std::vector<DecodedVideoFrame>& out)
{
    DecodedVideoView latest;
    std::size_t superseded = 0;
    if (!decode_latest(unit, pts_us, latest, superseded)) return false;
    if (!latest.frame) return true;
    DecodedVideoFrame owned{};
    if (!take_latest(owned)) return false;
    out.push_back(owned);
    return true;
}

std::string VideoDecoder::backend_name() const
{
    return impl_ ? impl_->backend : "unavailable";
}

void VideoDecoder::flush()
{
    if (!impl_) return;
    if (impl_->ctx) avcodec_flush_buffers(impl_->ctx);
    impl_->clear_latest();
    if (impl_->scratch) av_frame_unref(impl_->scratch);
    if (impl_->packet) av_packet_unref(impl_->packet);
}

VideoDecoder::~VideoDecoder()
{
    if (!impl_) return;
    impl_->reset_context();
    if (impl_->packet) av_packet_free(&impl_->packet);
    if (impl_->scratch) av_frame_free(&impl_->scratch);
    if (impl_->latest) av_frame_free(&impl_->latest);
    delete impl_;
    impl_ = nullptr;
}

}
