#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <icodecapi.h>
#include <oleauto.h>

#include <opal/video_encoder_backend.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <utility>

namespace opal {
namespace {

template <class T>
void release_com(T*& value)
{
    if (value) {
        value->Release();
        value = nullptr;
    }
}

std::string hresult_text(HRESULT value)
{
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "HRESULT 0x%08lx", static_cast<unsigned long>(value));
    return buffer;
}

PlatformError mf_error(HRESULT value, std::string message, bool fallback = false)
{
    PlatformError error;
    error.component = PlatformComponent::Encoder;
    error.failure = value == MF_E_TOPO_CODEC_NOT_FOUND ? PlatformFailure::Unavailable : PlatformFailure::OsError;
    error.message = std::move(message) + " (" + hresult_text(value) + ")";
    error.fallback_possible = fallback;
    return error;
}

bool set_codec_u32(ICodecAPI* codec, const GUID& key, std::uint32_t value)
{
    if (!codec) return false;
    VARIANT variant;
    VariantInit(&variant);
    variant.vt = VT_UI4;
    variant.ulVal = value;
    const HRESULT hr = codec->SetValue(&key, &variant);
    VariantClear(&variant);
    return SUCCEEDED(hr);
}

bool set_codec_bool(ICodecAPI* codec, const GUID& key, bool value)
{
    if (!codec) return false;
    VARIANT variant;
    VariantInit(&variant);
    variant.vt = VT_BOOL;
    variant.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    const HRESULT hr = codec->SetValue(&key, &variant);
    VariantClear(&variant);
    return SUCCEEDED(hr);
}

std::pair<int, int> output_dimensions(const StreamOptions& stream, int source_width, int source_height)
{
    if (source_width <= 0 || source_height <= 0) return {0, 0};
    if (stream.max_width <= 0 || stream.max_height <= 0) return {source_width & ~1, source_height & ~1};
    const double scale = std::min({1.0,
        static_cast<double>(stream.max_width) / static_cast<double>(source_width),
        static_cast<double>(stream.max_height) / static_cast<double>(source_height)});
    int width = std::max(16, static_cast<int>(std::lround(source_width * scale))) & ~1;
    int height = std::max(16, static_cast<int>(std::lround(source_height * scale))) & ~1;
    return {width, height};
}

bool sample_bytes(IMFSample* sample, std::vector<std::uint8_t>& bytes)
{
    if (!sample) return false;
    IMFMediaBuffer* buffer = nullptr;
    HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(hr) || !buffer) return false;
    BYTE* data = nullptr;
    DWORD max_length = 0;
    DWORD current_length = 0;
    hr = buffer->Lock(&data, &max_length, &current_length);
    if (SUCCEEDED(hr) && data && current_length > 0) bytes.assign(data, data + current_length);
    if (SUCCEEDED(hr)) buffer->Unlock();
    buffer->Release();
    return SUCCEEDED(hr) && !bytes.empty();
}

}

class WindowsVideoEncoderBackend final : public VideoEncoderBackend {
public:
    ~WindowsVideoEncoderBackend() override { stop(); }

    bool start(const StreamOptions& stream, int bitrate_kbps) override
    {
        stop();
        stream_ = stream;
        fps_ = std::clamp(stream.fps, 15, 240);
        bitrate_kbps_ = std::max(1000, bitrate_kbps);
        error_ = {};

        const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(com_hr)) com_initialized_ = true;
        else if (com_hr != RPC_E_CHANGED_MODE) {
            error_ = mf_error(com_hr, "COM initialization failed");
            return false;
        }
        const HRESULT mf_hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(mf_hr)) {
            error_ = mf_error(mf_hr, "Media Foundation startup failed");
            if (com_initialized_) { CoUninitialize(); com_initialized_ = false; }
            return false;
        }
        mf_started_ = true;
        running_ = true;
        return true;
    }

    bool encode(const NativeVideoFrame& frame, EncodedMediaUnit& unit) override
    {
        unit = {};
        if (!running_ || frame.kind != NativeVideoFrameKind::Opaque || !frame.opaque || frame.width <= 0 || frame.height <= 0) {
            error_ = {PlatformComponent::Encoder, PlatformFailure::InvalidState,
                      "Media Foundation encoder requires an opaque D3D11 capture frame", false};
            return false;
        }

        auto* input_texture = static_cast<ID3D11Texture2D*>(frame.opaque);
        if (!transform_ && !open_pipeline(input_texture, frame.width, frame.height)) return false;
        if (frame.width != source_width_ || frame.height != source_height_) {
            error_ = {PlatformComponent::Encoder, PlatformFailure::InvalidState,
                      "capture dimensions changed; media generation must restart", true};
            return false;
        }

        D3D11_TEXTURE2D_DESC input_desc{};
        input_texture->GetDesc(&input_desc);
        if (input_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && input_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
            error_ = {PlatformComponent::Encoder, PlatformFailure::Unsupported,
                      "Desktop Duplication frame format is not BGRA8", false};
            return false;
        }
        if (!convert_to_nv12(input_texture)) return false;

        const bool force_idr = force_idr_.exchange(false, std::memory_order_acq_rel);
        if (force_idr && codec_api_ && !set_codec_u32(codec_api_, CODECAPI_AVEncVideoForceKeyFrame, 1)) {
            error_ = {PlatformComponent::Encoder, PlatformFailure::OsError,
                      "hardware H.264 encoder rejected key-frame request", false};
            return false;
        }

        IMFSample* input_sample = nullptr;
        IMFMediaBuffer* surface_buffer = nullptr;
        HRESULT hr = MFCreateSample(&input_sample);
        if (SUCCEEDED(hr)) hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12_texture_, 0, FALSE, &surface_buffer);
        if (SUCCEEDED(hr)) hr = input_sample->AddBuffer(surface_buffer);
        release_com(surface_buffer);
        if (FAILED(hr) || !input_sample) {
            release_com(input_sample);
            error_ = mf_error(hr, "could not wrap NV12 D3D11 texture for Media Foundation");
            return false;
        }

        const LONGLONG time_100ns = static_cast<LONGLONG>((frame_index_ * 10000000ULL) / static_cast<std::uint64_t>(fps_));
        const LONGLONG duration_100ns = static_cast<LONGLONG>(10000000ULL / static_cast<std::uint64_t>(fps_));
        (void)input_sample->SetSampleTime(time_100ns);
        (void)input_sample->SetSampleDuration(duration_100ns);

        if (async_ && !wait_async_flag(need_input_, 1000)) {
            release_com(input_sample);
            error_ = {PlatformComponent::Encoder, PlatformFailure::Unavailable,
                      "hardware encoder did not request input in time", true};
            return false;
        }
        need_input_ = false;

        hr = transform_->ProcessInput(0, input_sample, 0);
        release_com(input_sample);
        if (FAILED(hr)) {
            error_ = mf_error(hr, "hardware encoder ProcessInput failed", true);
            return false;
        }

        if (async_ && !wait_async_flag(have_output_, 2000)) {
            error_ = {PlatformComponent::Encoder, PlatformFailure::Unavailable,
                      "hardware encoder did not produce output in time", true};
            return false;
        }
        have_output_ = false;

        IMFSample* encoded_sample = nullptr;
        hr = process_output(&encoded_sample);
        if (FAILED(hr) || !encoded_sample) {
            release_com(encoded_sample);
            error_ = mf_error(hr, "hardware encoder ProcessOutput failed", true);
            return false;
        }

        unit.kind = MediaKind::VideoH264;
        unit.pts_us = static_cast<std::int64_t>((frame_index_ * 1000000ULL) / static_cast<std::uint64_t>(fps_));
        unit.capture_time_us = frame.capture_time_us;
        UINT32 clean_point = FALSE;
        unit.keyframe = SUCCEEDED(encoded_sample->GetUINT32(MFSampleExtension_CleanPoint, &clean_point)) && clean_point != FALSE;
        const bool bytes_ok = sample_bytes(encoded_sample, unit.data);
        release_com(encoded_sample);
        if (!bytes_ok) {
            error_ = {PlatformComponent::Encoder, PlatformFailure::Unavailable,
                      "hardware encoder returned an empty H.264 sample", true};
            return false;
        }

        if (config_.extradata.empty()) refresh_sequence_header();
        ++frame_index_;
        return true;
    }

    void request_idr() override { force_idr_.store(true, std::memory_order_release); }

    bool set_bitrate(int bitrate_kbps) override
    {
        bitrate_kbps_ = std::max(1000, bitrate_kbps);
        if (!codec_api_) return true;
        const std::uint64_t requested = static_cast<std::uint64_t>(bitrate_kbps_) * 1000ULL;
        const auto bits = static_cast<std::uint32_t>(std::min<std::uint64_t>(requested, std::numeric_limits<std::uint32_t>::max()));
        if (!set_codec_u32(codec_api_, CODECAPI_AVEncCommonMeanBitRate, bits)) {
            error_ = {PlatformComponent::Encoder, PlatformFailure::OsError,
                      "hardware encoder rejected bitrate update", false};
            return false;
        }
        return true;
    }

    MediaConfig config() const override { return config_; }
    std::string backend_name() const override { return "media-foundation-hardware+d3d11-nv12-lowlatency"; }
    PlatformError last_platform_error() const override { return error_; }

    void stop() override
    {
        running_ = false;
        if (transform_) {
            (void)transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            (void)transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            (void)transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        release_com(output_sample_);
        release_com(event_generator_);
        release_com(codec_api_);
        release_com(transform_);
        release_com(device_manager_);
        release_video_processor();
        release_com(context_);
        release_com(device_);
        if (mf_started_) { MFShutdown(); mf_started_ = false; }
        if (com_initialized_) { CoUninitialize(); com_initialized_ = false; }
        source_width_ = source_height_ = output_width_ = output_height_ = 0;
        frame_index_ = 0;
        reset_token_ = 0;
        async_ = false;
        need_input_ = false;
        have_output_ = false;
        force_idr_.store(false, std::memory_order_release);
        config_ = {};
    }

private:
    bool open_pipeline(ID3D11Texture2D* input_texture, int width, int height)
    {
        if (!input_texture || !mf_started_) return false;
        input_texture->GetDevice(&device_);
        if (!device_) {
            error_ = {PlatformComponent::Encoder, PlatformFailure::InvalidState,
                      "capture texture has no D3D11 device", false};
            return false;
        }
        device_->GetImmediateContext(&context_);
        if (!context_) {
            error_ = {PlatformComponent::Encoder, PlatformFailure::InvalidState,
                      "capture D3D11 device has no immediate context", false};
            return false;
        }

        source_width_ = width;
        source_height_ = height;
        const auto output_size = output_dimensions(stream_, width, height);
        output_width_ = output_size.first;
        output_height_ = output_size.second;
        if (output_width_ <= 0 || output_height_ <= 0) return false;

        HRESULT hr = MFCreateDXGIDeviceManager(&reset_token_, &device_manager_);
        if (SUCCEEDED(hr)) hr = device_manager_->ResetDevice(device_, reset_token_);
        if (FAILED(hr)) {
            error_ = mf_error(hr, "could not create Media Foundation D3D11 device manager");
            return false;
        }
        if (!open_video_processor()) return false;
        if (!open_hardware_transform()) return false;
        return true;
    }

    bool open_video_processor()
    {
        HRESULT hr = device_->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(&video_device_));
        if (SUCCEEDED(hr)) hr = context_->QueryInterface(__uuidof(ID3D11VideoContext), reinterpret_cast<void**>(&video_context_));
        if (FAILED(hr) || !video_device_ || !video_context_) {
            error_ = mf_error(hr, "D3D11 video processor interfaces are unavailable");
            return false;
        }

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate.Numerator = static_cast<UINT>(fps_);
        content.InputFrameRate.Denominator = 1;
        content.InputWidth = static_cast<UINT>(source_width_);
        content.InputHeight = static_cast<UINT>(source_height_);
        content.OutputFrameRate.Numerator = static_cast<UINT>(fps_);
        content.OutputFrameRate.Denominator = 1;
        content.OutputWidth = static_cast<UINT>(output_width_);
        content.OutputHeight = static_cast<UINT>(output_height_);
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        hr = video_device_->CreateVideoProcessorEnumerator(&content, &video_enumerator_);
        if (SUCCEEDED(hr)) hr = video_device_->CreateVideoProcessor(video_enumerator_, 0, &video_processor_);
        if (FAILED(hr)) {
            error_ = mf_error(hr, "D3D11 video processor creation failed");
            return false;
        }

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(output_width_);
        desc.Height = static_cast<UINT>(output_height_);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        hr = device_->CreateTexture2D(&desc, nullptr, &nv12_texture_);
        if (FAILED(hr) || !nv12_texture_) {
            error_ = mf_error(hr, "could not allocate NV12 D3D11 encoder surface");
            return false;
        }

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_view_desc{};
        output_view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        output_view_desc.Texture2D.MipSlice = 0;
        hr = video_device_->CreateVideoProcessorOutputView(nv12_texture_, video_enumerator_,
                                                           &output_view_desc, &output_view_);
        if (FAILED(hr) || !output_view_) {
            error_ = mf_error(hr, "could not create NV12 video-processor output view");
            return false;
        }
        return true;
    }

    bool convert_to_nv12(ID3D11Texture2D* input_texture)
    {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
        input_desc.FourCC = 0;
        input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        input_desc.Texture2D.MipSlice = 0;
        input_desc.Texture2D.ArraySlice = 0;
        ID3D11VideoProcessorInputView* input_view = nullptr;
        HRESULT hr = video_device_->CreateVideoProcessorInputView(input_texture, video_enumerator_,
                                                                   &input_desc, &input_view);
        if (FAILED(hr) || !input_view) {
            error_ = mf_error(hr, "could not create Desktop Duplication video-processor input view");
            return false;
        }

        RECT source{0, 0, source_width_, source_height_};
        RECT destination{0, 0, output_width_, output_height_};
        video_context_->VideoProcessorSetStreamSourceRect(video_processor_, 0, TRUE, &source);
        video_context_->VideoProcessorSetStreamDestRect(video_processor_, 0, TRUE, &destination);
        video_context_->VideoProcessorSetOutputTargetRect(video_processor_, TRUE, &destination);

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.OutputIndex = 0;
        stream.InputFrameOrField = 0;
        stream.PastFrames = 0;
        stream.FutureFrames = 0;
        stream.pInputSurface = input_view;
        hr = video_context_->VideoProcessorBlt(video_processor_, output_view_, 0, 1, &stream);
        input_view->Release();
        if (FAILED(hr)) {
            error_ = mf_error(hr, "D3D11 BGRA-to-NV12 conversion failed", true);
            return false;
        }
        return true;
    }

    bool open_hardware_transform()
    {
        MFT_REGISTER_TYPE_INFO input_info{MFMediaType_Video, MFVideoFormat_NV12};
        MFT_REGISTER_TYPE_INFO output_info{MFMediaType_Video, MFVideoFormat_H264};
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        const UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER |
                             MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_LOCALMFT;
        HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &input_info, &output_info, &activates, &count);
        if (FAILED(hr) || count == 0 || !activates) {
            if (activates) CoTaskMemFree(activates);
            error_ = {PlatformComponent::Encoder, PlatformFailure::Unavailable,
                      "no hardware Media Foundation H.264 encoder is available", false};
            return false;
        }

        for (UINT32 i = 0; i < count && !transform_; ++i) {
            if (activates[i]) (void)activates[i]->ActivateObject(__uuidof(IMFTransform), reinterpret_cast<void**>(&transform_));
        }
        for (UINT32 i = 0; i < count; ++i) if (activates[i]) activates[i]->Release();
        CoTaskMemFree(activates);
        if (!transform_) {
            error_ = {PlatformComponent::Encoder, PlatformFailure::Unavailable,
                      "hardware H.264 MFT activation failed", false};
            return false;
        }

        IMFAttributes* attributes = nullptr;
        if (SUCCEEDED(transform_->GetAttributes(&attributes)) && attributes) {
            UINT32 is_async = FALSE;
            if (SUCCEEDED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &is_async)) && is_async) {
                if (FAILED(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE))) {
                    attributes->Release();
                    error_ = {PlatformComponent::Encoder, PlatformFailure::Unsupported,
                              "hardware encoder is asynchronous but cannot be unlocked", false};
                    return false;
                }
                async_ = true;
            }
            attributes->Release();
        }
        if (async_) {
            hr = transform_->QueryInterface(__uuidof(IMFMediaEventGenerator), reinterpret_cast<void**>(&event_generator_));
            if (FAILED(hr) || !event_generator_) {
                error_ = mf_error(hr, "asynchronous hardware encoder exposes no media event generator");
                return false;
            }
        }

        hr = transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                        reinterpret_cast<ULONG_PTR>(device_manager_));
        if (FAILED(hr)) {
            error_ = mf_error(hr, "hardware encoder rejected D3D11 device manager");
            return false;
        }
        (void)transform_->QueryInterface(__uuidof(ICodecAPI), reinterpret_cast<void**>(&codec_api_));
        if (codec_api_) {
            (void)set_codec_bool(codec_api_, CODECAPI_AVLowLatencyMode, true);
            (void)set_codec_u32(codec_api_, CODECAPI_AVEncCommonRateControlMode,
                                static_cast<std::uint32_t>(eAVEncCommonRateControlMode_CBR));
            const std::uint64_t requested = static_cast<std::uint64_t>(bitrate_kbps_) * 1000ULL;
            const auto bits = static_cast<std::uint32_t>(std::min<std::uint64_t>(requested, std::numeric_limits<std::uint32_t>::max()));
            (void)set_codec_u32(codec_api_, CODECAPI_AVEncCommonMeanBitRate, bits);
        }

        IMFMediaType* output_type = nullptr;
        hr = MFCreateMediaType(&output_type);
        if (SUCCEEDED(hr)) hr = output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr)) hr = output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        if (SUCCEEDED(hr)) hr = MFSetAttributeSize(output_type, MF_MT_FRAME_SIZE, output_width_, output_height_);
        if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(output_type, MF_MT_FRAME_RATE, fps_, 1);
        if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(output_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (SUCCEEDED(hr)) hr = output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (SUCCEEDED(hr)) hr = output_type->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrate_kbps_ * 1000));
        if (SUCCEEDED(hr)) hr = output_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        if (SUCCEEDED(hr)) hr = transform_->SetOutputType(0, output_type, 0);
        release_com(output_type);
        if (FAILED(hr)) {
            error_ = mf_error(hr, "hardware encoder rejected H.264 output type");
            return false;
        }

        IMFMediaType* input_type = nullptr;
        hr = MFCreateMediaType(&input_type);
        if (SUCCEEDED(hr)) hr = input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr)) hr = input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        if (SUCCEEDED(hr)) hr = MFSetAttributeSize(input_type, MF_MT_FRAME_SIZE, output_width_, output_height_);
        if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(input_type, MF_MT_FRAME_RATE, fps_, 1);
        if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(input_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (SUCCEEDED(hr)) hr = input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (SUCCEEDED(hr)) hr = transform_->SetInputType(0, input_type, 0);
        release_com(input_type);
        if (FAILED(hr)) {
            error_ = mf_error(hr, "hardware encoder rejected NV12 input type");
            return false;
        }

        MFT_OUTPUT_STREAM_INFO output_stream{};
        hr = transform_->GetOutputStreamInfo(0, &output_stream);
        if (FAILED(hr)) {
            error_ = mf_error(hr, "could not query hardware encoder output stream");
            return false;
        }
        output_provides_samples_ = (output_stream.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
        if (!output_provides_samples_) {
            hr = MFCreateSample(&output_sample_);
            IMFMediaBuffer* output_buffer = nullptr;
            const DWORD fallback_size = static_cast<DWORD>(std::max(1024 * 1024, output_width_ * output_height_));
            const DWORD capacity = std::max(output_stream.cbSize, fallback_size);
            if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer(capacity, &output_buffer);
            if (SUCCEEDED(hr)) hr = output_sample_->AddBuffer(output_buffer);
            release_com(output_buffer);
            if (FAILED(hr)) {
                error_ = mf_error(hr, "could not allocate hardware encoder output sample");
                return false;
            }
        }

        hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        if (SUCCEEDED(hr)) hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        if (FAILED(hr)) {
            error_ = mf_error(hr, "hardware encoder failed to start streaming");
            return false;
        }
        if (async_ && !wait_async_flag(need_input_, 1000)) {
            error_ = {PlatformComponent::Encoder, PlatformFailure::Unavailable,
                      "asynchronous hardware encoder did not request initial input", true};
            return false;
        }
        refresh_sequence_header();
        return true;
    }

    bool wait_async_flag(bool& flag, int timeout_ms)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!flag && std::chrono::steady_clock::now() < deadline) {
            IMFMediaEvent* event = nullptr;
            const HRESULT hr = event_generator_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) {
                std::this_thread::yield();
                continue;
            }
            if (FAILED(hr) || !event) return false;
            MediaEventType type = MEUnknown;
            HRESULT status = S_OK;
            (void)event->GetType(&type);
            (void)event->GetStatus(&status);
            event->Release();
            if (FAILED(status)) return false;
            if (type == METransformNeedInput) need_input_ = true;
            else if (type == METransformHaveOutput) have_output_ = true;
        }
        return flag;
    }

    HRESULT process_output(IMFSample** result)
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (output_sample_) {
            IMFMediaBuffer* buffer = nullptr;
            if (SUCCEEDED(output_sample_->GetBufferByIndex(0, &buffer)) && buffer) {
                (void)buffer->SetCurrentLength(0);
                buffer->Release();
            }
        }

        MFT_OUTPUT_DATA_BUFFER output{};
        output.dwStreamID = 0;
        output.pSample = output_provides_samples_ ? nullptr : output_sample_;
        DWORD status = 0;
        HRESULT hr = transform_->ProcessOutput(0, 1, &output, &status);
        if (output.pEvents) output.pEvents->Release();
        if (FAILED(hr)) {
            if (output_provides_samples_ && output.pSample) output.pSample->Release();
            return hr;
        }
        IMFSample* sample = output.pSample;
        if (!sample) return MF_E_TRANSFORM_NEED_MORE_INPUT;
        sample->AddRef();
        *result = sample;
        if (output_provides_samples_) sample->Release();
        return S_OK;
    }

    void refresh_sequence_header()
    {
        if (!transform_ || !config_.extradata.empty()) return;
        IMFMediaType* type = nullptr;
        if (FAILED(transform_->GetOutputCurrentType(0, &type)) || !type) return;
        UINT32 bytes = 0;
        if (SUCCEEDED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &bytes)) && bytes > 0) {
            config_.extradata.resize(bytes);
            UINT32 written = 0;
            if (FAILED(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, config_.extradata.data(), bytes, &written)) || written == 0)
                config_.extradata.clear();
            else
                config_.extradata.resize(written);
        }
        type->Release();
        config_.kind = MediaKind::VideoH264;
    }

    void release_video_processor()
    {
        release_com(output_view_);
        release_com(nv12_texture_);
        release_com(video_processor_);
        release_com(video_enumerator_);
        release_com(video_context_);
        release_com(video_device_);
    }

    StreamOptions stream_{};
    int source_width_ = 0;
    int source_height_ = 0;
    int output_width_ = 0;
    int output_height_ = 0;
    int fps_ = 60;
    int bitrate_kbps_ = 30000;
    std::uint64_t frame_index_ = 0;
    bool running_ = false;
    bool mf_started_ = false;
    bool com_initialized_ = false;
    bool async_ = false;
    bool need_input_ = false;
    bool have_output_ = false;
    bool output_provides_samples_ = false;
    std::atomic<bool> force_idr_{false};

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11VideoDevice* video_device_ = nullptr;
    ID3D11VideoContext* video_context_ = nullptr;
    ID3D11VideoProcessorEnumerator* video_enumerator_ = nullptr;
    ID3D11VideoProcessor* video_processor_ = nullptr;
    ID3D11Texture2D* nv12_texture_ = nullptr;
    ID3D11VideoProcessorOutputView* output_view_ = nullptr;

    IMFDXGIDeviceManager* device_manager_ = nullptr;
    UINT reset_token_ = 0;
    IMFTransform* transform_ = nullptr;
    IMFMediaEventGenerator* event_generator_ = nullptr;
    ICodecAPI* codec_api_ = nullptr;
    IMFSample* output_sample_ = nullptr;

    MediaConfig config_{};
    PlatformError error_{};
};

std::unique_ptr<VideoEncoderBackend> make_video_encoder_backend()
{
    return std::make_unique<WindowsVideoEncoderBackend>();
}

}
