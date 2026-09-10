#include <opal/linux_video_backend.hpp>
#include <opal/video_encoder_backend.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace opal {
namespace {

std::vector<std::uint8_t> parameter_sets(std::span<const std::uint8_t> data)
{
    std::vector<std::uint8_t> result;
    bool have_sps = false;
    bool have_pps = false;
    std::size_t position = 0;

    auto find_start = [&](std::size_t from, std::size_t& begin, std::size_t& prefix) {
        for (std::size_t i = from; i + 3 <= data.size(); ++i) {
            if (data[i] != 0 || data[i + 1] != 0) continue;
            if (data[i + 2] == 1) { begin = i; prefix = 3; return true; }
            if (i + 4 <= data.size() && data[i + 2] == 0 && data[i + 3] == 1) {
                begin = i; prefix = 4; return true;
            }
        }
        return false;
    };

    for (;;) {
        std::size_t begin = 0;
        std::size_t prefix = 0;
        if (!find_start(position, begin, prefix)) break;
        const std::size_t nal = begin + prefix;
        if (nal >= data.size()) break;
        std::size_t next = 0;
        std::size_t next_prefix = 0;
        const bool more = find_start(nal + 1, next, next_prefix);
        const std::size_t end = more ? next : data.size();
        const std::uint8_t type = data[nal] & 0x1f;
        if (type == 7 || type == 8) {
            result.insert(result.end(), data.begin() + static_cast<std::ptrdiff_t>(begin),
                          data.begin() + static_cast<std::ptrdiff_t>(end));
            have_sps |= type == 7;
            have_pps |= type == 8;
        }
        if (!more) break;
        position = next;
        (void)next_prefix;
    }
    if (!have_sps || !have_pps) result.clear();
    return result;
}

class LinuxVideoEncoderBackend final : public VideoEncoderBackend {
public:
    ~LinuxVideoEncoderBackend() override { stop(); }

    bool start(const StreamOptions& stream, int bitrate_kbps) override
    {
        stop();
        fps_ = std::clamp(stream.fps, 15, 240);
        bitrate_kbps_ = std::max(1000, bitrate_kbps);
        error_ = {};
        force_idr_.store(true, std::memory_order_release);
        running_ = true;
        return true;
    }

    bool encode(const NativeVideoFrame& frame, EncodedMediaUnit& unit) override
    {
        unit = {};
        if (!running_ || frame.kind != NativeVideoFrameKind::Cpu || frame.bytes.empty()) return false;
        const auto input_format = static_cast<AVPixelFormat>(frame.pixel_format);
        if (!ctx_ || frame.width != width_ || frame.height != height_ || input_format != input_format_) {
            if (!configure(frame, input_format)) return false;
        }

        if (av_frame_make_writable(sw_frame_) < 0) return fail("Linux H.264 input frame is not writable");
        const std::uint8_t* source[4] = {frame.bytes.data(), nullptr, nullptr, nullptr};
        int source_stride[4] = {frame.stride, 0, 0, 0};
        if (sws_scale(sws_, source, source_stride, 0, frame.height, sw_frame_->data, sw_frame_->linesize) <= 0)
            return fail("Linux H.264 pixel conversion failed");

        sw_frame_->pts = static_cast<std::int64_t>(frame_index_++);
        sw_frame_->pict_type = force_idr_.exchange(false, std::memory_order_acq_rel) ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;

        AVFrame* submitted = sw_frame_;
        if (hardware_) {
            av_frame_unref(hw_frame_);
            if (av_hwframe_get_buffer(ctx_->hw_frames_ctx, hw_frame_, 0) < 0 ||
                av_hwframe_transfer_data(hw_frame_, sw_frame_, 0) < 0)
                return fail("Linux VAAPI upload failed");
            hw_frame_->pts = sw_frame_->pts;
            hw_frame_->pict_type = sw_frame_->pict_type;
            submitted = hw_frame_;
        }

        if (avcodec_send_frame(ctx_, submitted) < 0) return fail("Linux H.264 encoder rejected a frame");
        const int rc = avcodec_receive_packet(ctx_, packet_);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return false;
        if (rc < 0) return fail("Linux H.264 encoder failed to return a packet");

        const bool keyframe = (packet_->flags & AV_PKT_FLAG_KEY) != 0;
        unit.kind = MediaKind::VideoH264;
        unit.data.assign(packet_->data, packet_->data + packet_->size);
        unit.pts_us = static_cast<std::int64_t>(frame.capture_time_us);
        unit.capture_time_us = frame.capture_time_us;
        unit.keyframe = keyframe;
        publish_config(std::span<const std::uint8_t>(packet_->data, packet_->size), keyframe);
        av_packet_unref(packet_);
        return !unit.data.empty();
    }

    void request_idr() override
    {
        force_idr_.store(true, std::memory_order_release);
    }

    bool set_bitrate(int bitrate_kbps) override
    {
        bitrate_kbps_ = std::max(1000, bitrate_kbps);
        if (ctx_) ctx_->bit_rate = static_cast<std::int64_t>(bitrate_kbps_) * 1000;
        return running_;
    }

    MediaConfig config() const override { return config_; }
    std::string backend_name() const override { return name_.empty() ? "ffmpeg-h264" : name_; }
    PlatformError last_platform_error() const override { return error_; }

    void stop() override
    {
        running_ = false;
        release_codec();
        config_ = {};
        error_ = {};
        width_ = 0;
        height_ = 0;
        input_format_ = AV_PIX_FMT_NONE;
        frame_index_ = 0;
        name_.clear();
        force_idr_.store(false, std::memory_order_release);
    }

private:
    static bool accepts_format(const AVCodec* codec, AVPixelFormat& format)
    {
        if (!codec) return false;
        const AVPixelFormat* formats = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61,12,100)
        int count = 0;
        if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                         reinterpret_cast<const void**>(&formats), &count) < 0)
            return false;
        if (!formats) { format = AV_PIX_FMT_YUV420P; return true; }
        for (int i = 0; i < count; ++i) if (formats[i] == AV_PIX_FMT_NV12) { format = AV_PIX_FMT_NV12; return true; }
        for (int i = 0; i < count; ++i) if (formats[i] == AV_PIX_FMT_YUV420P) { format = AV_PIX_FMT_YUV420P; return true; }
#else
        formats = codec->pix_fmts;
        if (!formats) { format = AV_PIX_FMT_YUV420P; return true; }
        for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p)
            if (*p == AV_PIX_FMT_NV12) { format = *p; return true; }
        for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p)
            if (*p == AV_PIX_FMT_YUV420P) { format = *p; return true; }
#endif
        return false;
    }

    void configure_common(const NativeVideoFrame& frame, AVPixelFormat codec_format)
    {
        ctx_->width = frame.width;
        ctx_->height = frame.height;
        ctx_->pix_fmt = codec_format;
        ctx_->time_base = AVRational{1, fps_};
        ctx_->framerate = AVRational{fps_, 1};
        ctx_->bit_rate = static_cast<std::int64_t>(bitrate_kbps_) * 1000;
        ctx_->gop_size = std::max(fps_, 30);
        ctx_->max_b_frames = 0;
        ctx_->thread_count = 1;
        ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY | AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    bool allocate_cpu_frame(const NativeVideoFrame& frame, AVPixelFormat format)
    {
        sw_frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        if (!sw_frame_ || !packet_) return false;
        sw_frame_->format = format;
        sw_frame_->width = frame.width;
        sw_frame_->height = frame.height;
        if (av_frame_get_buffer(sw_frame_, 32) < 0) return false;
        sws_ = sws_getContext(frame.width, frame.height, static_cast<AVPixelFormat>(frame.pixel_format),
                              frame.width, frame.height, format, SWS_FAST_BILINEAR,
                              nullptr, nullptr, nullptr);
        return sws_ != nullptr;
    }

    bool open_vaapi(const NativeVideoFrame& frame)
    {
        const AVCodec* codec = avcodec_find_encoder_by_name("h264_vaapi");
        if (!codec || av_hwdevice_ctx_create(&hw_device_, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) < 0 || !hw_device_)
            return false;
        ctx_ = avcodec_alloc_context3(codec);
        if (!ctx_) return false;
        configure_common(frame, AV_PIX_FMT_VAAPI);

        AVBufferRef* frames_ref = av_hwframe_ctx_alloc(hw_device_);
        if (!frames_ref) return false;
        auto* frames = reinterpret_cast<AVHWFramesContext*>(frames_ref->data);
        frames->format = AV_PIX_FMT_VAAPI;
        frames->sw_format = AV_PIX_FMT_NV12;
        frames->width = frame.width;
        frames->height = frame.height;
        frames->initial_pool_size = 4;
        if (av_hwframe_ctx_init(frames_ref) < 0) {
            av_buffer_unref(&frames_ref);
            return false;
        }
        ctx_->hw_frames_ctx = av_buffer_ref(frames_ref);
        av_buffer_unref(&frames_ref);
        if (!ctx_->hw_frames_ctx) return false;

        AVDictionary* options = nullptr;
        av_dict_set(&options, "async_depth", "1", 0);
        const int rc = avcodec_open2(ctx_, codec, &options);
        av_dict_free(&options);
        if (rc < 0 || !allocate_cpu_frame(frame, AV_PIX_FMT_NV12)) return false;
        hw_frame_ = av_frame_alloc();
        if (!hw_frame_) return false;
        hardware_ = true;
        name_ = "h264_vaapi+hw";
        return true;
    }

    bool open_cpu_encoder(const NativeVideoFrame& frame)
    {
        constexpr const char* candidates[] = {"h264_nvenc", "h264_qsv", "libx264", "libopenh264"};
        for (const char* candidate : candidates) {
            const AVCodec* codec = avcodec_find_encoder_by_name(candidate);
            AVPixelFormat format = AV_PIX_FMT_NONE;
            if (!accepts_format(codec, format)) continue;
            ctx_ = avcodec_alloc_context3(codec);
            if (!ctx_) continue;
            configure_common(frame, format);
            AVDictionary* options = nullptr;
            const std::string candidate_name(candidate);
            if (candidate_name == "h264_nvenc") {
                av_dict_set(&options, "preset", "p1", 0);
                av_dict_set(&options, "tune", "ull", 0);
                av_dict_set(&options, "zerolatency", "1", 0);
                av_dict_set(&options, "delay", "0", 0);
            } else if (candidate_name == "h264_qsv") {
                av_dict_set(&options, "async_depth", "1", 0);
                av_dict_set(&options, "preset", "veryfast", 0);
            } else if (candidate_name == "libx264") {
                av_dict_set(&options, "preset", "ultrafast", 0);
                av_dict_set(&options, "tune", "zerolatency", 0);
            }
            const int rc = avcodec_open2(ctx_, codec, &options);
            av_dict_free(&options);
            if (rc >= 0 && allocate_cpu_frame(frame, format)) {
                hardware_ = candidate_name != "libx264" && candidate_name != "libopenh264";
                name_ = candidate_name + (hardware_ ? "+hw" : "+sw");
                return true;
            }
            release_codec();
        }
        return false;
    }

    bool configure(const NativeVideoFrame& frame, AVPixelFormat input_format)
    {
        release_codec();
        config_ = {};
        if (!open_vaapi(frame)) {
            release_codec();
            if (!open_cpu_encoder(frame)) return fail("no usable Linux low-latency H.264 encoder");
        }
        width_ = frame.width;
        height_ = frame.height;
        input_format_ = input_format;
        frame_index_ = 0;
        force_idr_.store(true, std::memory_order_release);
        if (ctx_->extradata && ctx_->extradata_size > 0) {
            config_.kind = MediaKind::VideoH264;
            config_.extradata.assign(ctx_->extradata, ctx_->extradata + ctx_->extradata_size);
        }
        return true;
    }

    void publish_config(std::span<const std::uint8_t> packet, bool keyframe)
    {
        if (!config_.extradata.empty()) return;
        auto extra = keyframe ? parameter_sets(packet) : std::vector<std::uint8_t>{};
        if (extra.empty()) return;
        config_.kind = MediaKind::VideoH264;
        config_.extradata = std::move(extra);
    }

    bool fail(std::string message)
    {
        error_ = {PlatformComponent::Encoder, PlatformFailure::OsError, std::move(message), true};
        return false;
    }

    void release_codec()
    {
        if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
        if (packet_) av_packet_free(&packet_);
        if (sw_frame_) av_frame_free(&sw_frame_);
        if (hw_frame_) av_frame_free(&hw_frame_);
        if (ctx_) avcodec_free_context(&ctx_);
        av_buffer_unref(&hw_device_);
        hardware_ = false;
    }

    AVCodecContext* ctx_ = nullptr;
    AVFrame* sw_frame_ = nullptr;
    AVFrame* hw_frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* sws_ = nullptr;
    AVBufferRef* hw_device_ = nullptr;

    int fps_ = 60;
    int bitrate_kbps_ = 30000;
    int width_ = 0;
    int height_ = 0;
    AVPixelFormat input_format_ = AV_PIX_FMT_NONE;
    std::uint64_t frame_index_ = 0;
    bool running_ = false;
    bool hardware_ = false;
    std::atomic<bool> force_idr_{false};
    std::string name_;
    MediaConfig config_{};
    PlatformError error_{};
};

}

std::unique_ptr<VideoEncoderBackend> make_linux_video_encoder_backend()
{
    return std::make_unique<LinuxVideoEncoderBackend>();
}

}
