#include <opal/native_video_pipeline.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#endif

#include <algorithm>
#include <chrono>
#include <utility>

namespace opal {
namespace {

bool same_config(const MediaConfig& a, const MediaConfig& b)
{
    return a.kind == b.kind && a.extradata == b.extradata &&
           a.sample_rate == b.sample_rate && a.channels == b.channels;
}

#if defined(_WIN32)
constexpr std::uint64_t kIddIdleFrameIntervalUs = 100000;

std::uint64_t monotonic_us()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

template <class T>
void release_windows_com(T*& value)
{
    if (value) value->Release();
    value = nullptr;
}

struct WindowsIddCopyCache {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D* texture = nullptr;
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::uint64_t last_emit_us = 0;

    ~WindowsIddCopyCache() { reset(); }

    void reset()
    {
        release_windows_com(texture);
        release_windows_com(context);
        release_windows_com(device);
        width = 0;
        height = 0;
        format = DXGI_FORMAT_UNKNOWN;
        last_emit_us = 0;
    }

    bool copy_and_release(NativeVideoFrame& frame, PlatformError& error)
    {
        auto* source = static_cast<ID3D11Texture2D*>(frame.opaque);
        if (!source) return false;

        D3D11_TEXTURE2D_DESC source_desc{};
        source->GetDesc(&source_desc);
        ID3D11Device* source_device = nullptr;
        source->GetDevice(&source_device);
        if (!source_device) {
            error = {PlatformComponent::Capture, PlatformFailure::InvalidState,
                     "OPAL IDD DXGI frame has no D3D11 device", true};
            return false;
        }

        const bool recreate = !device || device != source_device || !texture ||
                              width != source_desc.Width || height != source_desc.Height ||
                              format != source_desc.Format;
        if (recreate) {
            reset();
            device = source_device;
            source_device = nullptr;
            device->GetImmediateContext(&context);
            if (!context) {
                error = {PlatformComponent::Capture, PlatformFailure::InvalidState,
                         "OPAL IDD D3D11 device has no immediate context", true};
                reset();
                return false;
            }

            D3D11_TEXTURE2D_DESC copy_desc{};
            copy_desc.Width = source_desc.Width;
            copy_desc.Height = source_desc.Height;
            copy_desc.MipLevels = 1;
            copy_desc.ArraySize = 1;
            copy_desc.Format = source_desc.Format;
            copy_desc.SampleDesc.Count = 1;
            copy_desc.Usage = D3D11_USAGE_DEFAULT;
            copy_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            const HRESULT hr = device->CreateTexture2D(&copy_desc, nullptr, &texture);
            if (FAILED(hr) || !texture) {
                error = {PlatformComponent::Capture, PlatformFailure::OsError,
                         "could not allocate GPU copy for OPAL IDD frame", true};
                reset();
                return false;
            }
            width = source_desc.Width;
            height = source_desc.Height;
            format = source_desc.Format;
        }
        release_windows_com(source_device);

        context->CopyResource(texture, source);
        texture->AddRef();
        auto copy_owner = std::shared_ptr<void>(texture, [](void* value) {
            static_cast<ID3D11Texture2D*>(value)->Release();
        });

        frame.opaque = texture;
        frame.owner = std::move(copy_owner);
        last_emit_us = monotonic_us();
        return true;
    }

    bool repeat_if_due(NativeVideoFrame& frame)
    {
        if (!texture || width == 0 || height == 0 || format == DXGI_FORMAT_UNKNOWN) return false;
        const auto now = monotonic_us();
        if (last_emit_us && now < last_emit_us + kIddIdleFrameIntervalUs) return false;

        texture->AddRef();
        frame.kind = NativeVideoFrameKind::Opaque;
        frame.width = static_cast<int>(width);
        frame.height = static_cast<int>(height);
        frame.pixel_format = static_cast<std::uint32_t>(format);
        frame.capture_time_us = now;
        frame.opaque = texture;
        frame.owner = std::shared_ptr<void>(texture, [](void* value) {
            static_cast<ID3D11Texture2D*>(value)->Release();
        });
        last_emit_us = now;
        return true;
    }
};

WindowsIddCopyCache& idd_copy_cache()
{
    static thread_local WindowsIddCopyCache cache;
    return cache;
}

bool detach_idd_duplication_frame(CaptureBackend* capture, NativeVideoFrame& frame,
                                  PlatformError& error)
{
    if (!capture || capture->backend_name() != "iddcx-targeted-dxgi+d3d11") return true;
    return idd_copy_cache().copy_and_release(frame, error);
}

bool repeat_idd_idle_frame(CaptureBackend* capture, NativeVideoFrame& frame)
{
    return capture && capture->backend_name() == "iddcx-targeted-dxgi+d3d11" &&
           idd_copy_cache().repeat_if_due(frame);
}

void reset_idd_copy_cache()
{
    idd_copy_cache().reset();
}

bool idd_dxgi_access_lost(CaptureBackend* capture, const PlatformError& error)
{
    return capture && error &&
           capture->backend_name() == "iddcx-targeted-dxgi+d3d11" &&
           error.message.find("HRESULT 0x887a0026") != std::string::npos;
}
#endif

}

NativeVideoPipeline::NativeVideoPipeline(std::unique_ptr<CaptureBackend> capture,
                                         std::unique_ptr<VideoEncoderBackend> encoder)
    : capture_(std::move(capture)), encoder_(std::move(encoder))
{
}

NativeVideoPipeline::~NativeVideoPipeline()
{
    stop();
}

bool NativeVideoPipeline::start(const StreamOptions& stream, int bitrate_kbps, const DisplayTarget* target)
{
    stop();
    terminal_ = false;
    pipeline_error_ = {};
    if (!capture_ || !encoder_) return false;

#if defined(_WIN32)
    reset_idd_copy_cache();
#endif
    stream_options_ = stream;
    bitrate_kbps_ = std::max(1000, bitrate_kbps);
    has_display_target_ = target != nullptr;
    display_target_ = target ? *target : DisplayTarget{};

    if (!capture_->start(stream_options_, has_display_target_ ? &display_target_ : nullptr)) {
        terminal_ = static_cast<bool>(capture_->last_platform_error());
        return false;
    }
    if (!encoder_->start(stream_options_, bitrate_kbps_)) {
        terminal_ = static_cast<bool>(encoder_->last_platform_error());
        capture_->stop();
        return false;
    }
    config_ = {};
    config_revision_ = 0;
    first_frame_ = true;
    running_ = true;
    encoder_->request_idr();
    return true;
}

bool NativeVideoPipeline::next(EncodedMediaUnit& unit, int timeout_ms)
{
    unit = {};
    if (!running_ || terminal_ || !capture_ || !encoder_) return false;
    NativeVideoFrame frame;
    bool repeated_idd_frame = false;
    if (!capture_->next(frame, timeout_ms)) {
        const auto capture_error = capture_->last_platform_error();
#if defined(_WIN32)
        if (idd_dxgi_access_lost(capture_.get(), capture_error)) {
            capture_->stop();
            encoder_->stop();
            reset_idd_copy_cache();
            pipeline_error_ = {};

            if (!capture_->start(stream_options_, has_display_target_ ? &display_target_ : nullptr)) {
                pipeline_error_ = capture_->last_platform_error();
                terminal_ = true;
                running_ = false;
                return false;
            }
            if (!encoder_->start(stream_options_, bitrate_kbps_)) {
                pipeline_error_ = encoder_->last_platform_error();
                capture_->stop();
                terminal_ = true;
                running_ = false;
                return false;
            }

            config_ = {};
            first_frame_ = true;
            terminal_ = false;
            running_ = true;
            encoder_->request_idr();
            return false;
        }
        if (!capture_error && repeat_idd_idle_frame(capture_.get(), frame)) {
            repeated_idd_frame = true;
        } else
#endif
        {
            if (capture_error) {
                terminal_ = true;
                running_ = false;
            }
            return false;
        }
    }
#if defined(_WIN32)
    if (!repeated_idd_frame && !detach_idd_duplication_frame(capture_.get(), frame, pipeline_error_)) {
        terminal_ = true;
        running_ = false;
        return false;
    }
    if (first_frame_) frame.capture_time_us = monotonic_us();
#endif
    first_frame_ = false;
    if (!encoder_->encode(frame, unit)) {
        if (encoder_->last_platform_error()) {
            terminal_ = true;
            running_ = false;
        }
        return false;
    }
    const auto next_config = encoder_->config();
    if (!next_config.extradata.empty() && !same_config(config_, next_config)) {
        config_ = next_config;
        ++config_revision_;
    }
    return !unit.data.empty();
}

void NativeVideoPipeline::request_idr()
{
    if (encoder_ && running_ && !terminal_) encoder_->request_idr();
}

bool NativeVideoPipeline::set_bitrate(int bitrate_kbps)
{
    if (!encoder_ || !running_ || terminal_) return false;
    if (encoder_->set_bitrate(bitrate_kbps)) {
        bitrate_kbps_ = std::max(1000, bitrate_kbps);
        return true;
    }
    if (encoder_->last_platform_error()) {
        terminal_ = true;
        running_ = false;
    }
    return false;
}

void NativeVideoPipeline::stop()
{
    running_ = false;
    if (encoder_) encoder_->stop();
    if (capture_) capture_->stop();
#if defined(_WIN32)
    reset_idd_copy_cache();
#endif
    config_ = {};
    config_revision_ = 0;
    terminal_ = false;
    first_frame_ = true;
    pipeline_error_ = {};
}

const MediaConfig& NativeVideoPipeline::config() const
{
    return config_;
}

std::uint64_t NativeVideoPipeline::config_revision() const
{
    return config_revision_;
}

CaptureTimestampQuality NativeVideoPipeline::timestamp_quality() const
{
    return capture_ ? capture_->timestamp_quality() : CaptureTimestampQuality::Estimated;
}

std::string NativeVideoPipeline::backend_name() const
{
    if (!capture_ || !encoder_) return "unavailable";
    return capture_->backend_name() + "+" + encoder_->backend_name();
}

PlatformError NativeVideoPipeline::last_platform_error() const
{
    if (pipeline_error_) return pipeline_error_;
    if (encoder_) {
        auto error = encoder_->last_platform_error();
        if (error) return error;
    }
    return capture_ ? capture_->last_platform_error() : PlatformError{};
}

bool NativeVideoPipeline::ended() const noexcept
{
    return terminal_;
}

}
