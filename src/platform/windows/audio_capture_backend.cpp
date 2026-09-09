#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <ksmedia.h>

#include <opal/audio_capture_backend.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace opal {
namespace {
using Clock = std::chrono::steady_clock;

template <class T>
void release_com(T*& value)
{
    if (value) {
        value->Release();
        value = nullptr;
    }
}

std::uint64_t monotonic_us()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now().time_since_epoch()).count());
}

PlatformError audio_error(HRESULT value, std::string message, bool fallback = false)
{
    PlatformError error;
    error.component = PlatformComponent::AudioCapture;
    error.failure = value == AUDCLNT_E_DEVICE_INVALIDATED ? PlatformFailure::Unavailable : PlatformFailure::OsError;
    char suffix[32]{};
    std::snprintf(suffix, sizeof(suffix), " (HRESULT 0x%08lx)", static_cast<unsigned long>(value));
    error.message = std::move(message) + suffix;
    error.fallback_possible = fallback;
    return error;
}

AVSampleFormat wave_sample_format(const WAVEFORMATEX* format)
{
    if (!format) return AV_SAMPLE_FMT_NONE;
    WORD tag = format->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        if (IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) tag = WAVE_FORMAT_IEEE_FLOAT;
        else if (IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) tag = WAVE_FORMAT_PCM;
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32) return AV_SAMPLE_FMT_FLT;
    if (tag == WAVE_FORMAT_PCM && format->wBitsPerSample == 16) return AV_SAMPLE_FMT_S16;
    if (tag == WAVE_FORMAT_PCM && format->wBitsPerSample == 32) return AV_SAMPLE_FMT_S32;
    return AV_SAMPLE_FMT_NONE;
}

std::uint64_t qpc_capture_time_us(UINT64 qpc_100ns, std::uint64_t callback_us)
{
    if (qpc_100ns == 0) return callback_us;
    LARGE_INTEGER now{};
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceCounter(&now) || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
        return callback_us;
    const long double now_100ns = static_cast<long double>(now.QuadPart) * 10000000.0L /
                                  static_cast<long double>(frequency.QuadPart);
    if (now_100ns < static_cast<long double>(qpc_100ns)) return callback_us;
    const long double age_us = (now_100ns - static_cast<long double>(qpc_100ns)) / 10.0L;
    if (age_us < 0.0L || age_us > static_cast<long double>(callback_us)) return callback_us;
    return callback_us - static_cast<std::uint64_t>(age_us);
}

}

class WindowsAudioCaptureBackend final : public AudioCaptureBackend {
public:
    ~WindowsAudioCaptureBackend() override { stop(); }

    bool start() override
    {
        stop();
        error_ = {};

        const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(com_hr)) com_initialized_ = true;
        else if (com_hr != RPC_E_CHANGED_MODE) {
            error_ = audio_error(com_hr, "COM initialization failed for WASAPI loopback");
            return false;
        }

        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator_));
        if (SUCCEEDED(hr)) hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
        if (SUCCEEDED(hr)) hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                                  reinterpret_cast<void**>(&audio_client_));
        if (FAILED(hr) || !audio_client_) {
            error_ = audio_error(hr, "could not open the default Windows render endpoint", true);
            stop();
            return false;
        }

        hr = audio_client_->GetMixFormat(&mix_format_);
        if (FAILED(hr) || !mix_format_) {
            error_ = audio_error(hr, "WASAPI did not provide a shared-mode mix format", true);
            stop();
            return false;
        }
        input_format_ = wave_sample_format(mix_format_);
        input_rate_ = static_cast<int>(mix_format_->nSamplesPerSec);
        input_channels_ = static_cast<int>(mix_format_->nChannels);
        input_block_align_ = static_cast<int>(mix_format_->nBlockAlign);
        if (input_format_ == AV_SAMPLE_FMT_NONE || input_rate_ <= 0 || input_channels_ <= 0 || input_block_align_ <= 0) {
            error_ = {PlatformComponent::AudioCapture, PlatformFailure::Unsupported,
                      "WASAPI mix format is not supported by OPAL audio capture", false};
            stop();
            return false;
        }

        const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                            AUDCLNT_STREAMFLAGS_NOPERSIST;
        hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, mix_format_, nullptr);
        if (SUCCEEDED(hr)) hr = audio_client_->GetBufferSize(&buffer_frame_count_);
        if (FAILED(hr) || buffer_frame_count_ == 0) {
            error_ = audio_error(hr, "WASAPI loopback initialization or buffer query failed", true);
            stop();
            return false;
        }

        silence_.resize(static_cast<std::size_t>(buffer_frame_count_) * static_cast<std::size_t>(input_block_align_));

        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_) {
            error_ = {PlatformComponent::AudioCapture, PlatformFailure::OsError,
                      "could not create WASAPI capture event", true};
            stop();
            return false;
        }
        hr = audio_client_->SetEventHandle(event_);
        if (SUCCEEDED(hr)) hr = audio_client_->GetService(__uuidof(IAudioCaptureClient),
                                                          reinterpret_cast<void**>(&capture_client_));
        if (FAILED(hr) || !capture_client_) {
            error_ = audio_error(hr, "could not attach WASAPI loopback capture client", true);
            stop();
            return false;
        }
        if (!open_encoder()) {
            stop();
            return false;
        }

        hr = audio_client_->Start();
        if (FAILED(hr)) {
            error_ = audio_error(hr, "WASAPI loopback stream failed to start", true);
            stop();
            return false;
        }
        started_client_ = true;
        running_ = true;
        return true;
    }

    bool next(EncodedMediaUnit& unit, int timeout_ms) override
    {
        unit.data.clear();
        unit.kind = MediaKind::AudioAac;
        unit.pts_us = 0;
        unit.capture_time_us = 0;
        unit.keyframe = false;
        if (!running_ || !capture_client_ || !codec_ || !fifo_ || !encode_frame_) return false;
        const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(0, timeout_ms));

        for (;;) {
            if (av_audio_fifo_size(fifo_) >= codec_->frame_size) {
                if (!encode_one(unit)) return false;
                if (!unit.data.empty()) return true;
                continue;
            }

            const auto now = Clock::now();
            const DWORD remaining = timeout_ms <= 0 ? 0u : static_cast<DWORD>(std::max<std::int64_t>(
                0, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count()));
            const DWORD wait = WaitForSingleObject(event_, remaining);
            if (wait == WAIT_TIMEOUT) return false;
            if (wait != WAIT_OBJECT_0) {
                error_ = {PlatformComponent::AudioCapture, PlatformFailure::OsError,
                          "WASAPI capture event wait failed", true};
                running_ = false;
                return false;
            }
            if (!drain_capture_packets()) return false;
            if (timeout_ms <= 0 && av_audio_fifo_size(fifo_) < codec_->frame_size) return false;
            if (timeout_ms > 0 && Clock::now() >= deadline && av_audio_fifo_size(fifo_) < codec_->frame_size) return false;
        }
    }

    void stop() override
    {
        running_ = false;
        if (audio_client_ && started_client_) (void)audio_client_->Stop();
        started_client_ = false;
        reset_encoder();
        release_com(capture_client_);
        if (event_) { CloseHandle(event_); event_ = nullptr; }
        if (mix_format_) { CoTaskMemFree(mix_format_); mix_format_ = nullptr; }
        release_com(audio_client_);
        release_com(device_);
        release_com(enumerator_);
        if (com_initialized_) { CoUninitialize(); com_initialized_ = false; }
        input_rate_ = input_channels_ = input_block_align_ = 0;
        buffer_frame_count_ = 0;
        input_format_ = AV_SAMPLE_FMT_NONE;
        config_ = {};
        config_revision_ = 0;
        pending_capture_times_.clear();
        fifo_capture_us_ = 0;
        samples_submitted_ = 0;
        silence_.clear();
    }

    MediaConfig config() const override { return config_; }
    std::uint64_t config_revision() const override { return config_revision_; }
    std::string backend_name() const override { return "wasapi-loopback+aac"; }
    PlatformError last_platform_error() const override { return error_; }

private:
    bool allocate_resample_buffer(int capacity)
    {
        if (capacity <= 0) return false;
        std::uint8_t** replacement = nullptr;
        int linesize = 0;
        if (av_samples_alloc_array_and_samples(&replacement, &linesize,
                                               codec_->ch_layout.nb_channels, capacity,
                                               codec_->sample_fmt, 0) < 0 || !replacement) {
            if (replacement) av_freep(&replacement);
            return false;
        }
        if (resample_data_) {
            av_freep(&resample_data_[0]);
            av_freep(&resample_data_);
        }
        resample_data_ = replacement;
        resample_linesize_ = linesize;
        resample_capacity_ = capacity;
        return true;
    }

    bool ensure_resample_capacity(int capacity)
    {
        if (capacity <= resample_capacity_) return true;
        return allocate_resample_buffer(std::max(capacity, resample_capacity_ * 2));
    }

    bool ensure_fifo_space(int samples)
    {
        if (!fifo_ || samples <= 0) return samples <= 0;
        if (av_audio_fifo_space(fifo_) >= samples) return true;
        const int required = av_audio_fifo_size(fifo_) + samples;
        const int grown = std::max(required, std::max(8192, required * 2));
        return av_audio_fifo_realloc(fifo_, grown) >= 0;
    }

    bool open_encoder()
    {
        const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!encoder) {
            error_ = {PlatformComponent::AudioCapture, PlatformFailure::DependencyMissing,
                      "linked libavcodec has no AAC encoder", false};
            return false;
        }
        codec_ = avcodec_alloc_context3(encoder);
        if (!codec_) return false;
        codec_->sample_rate = input_rate_;
        const int output_channels = input_channels_ == 1 ? 1 : 2;
        av_channel_layout_default(&codec_->ch_layout, output_channels);
        codec_->time_base = AVRational{1, input_rate_};
        codec_->bit_rate = 128000;
        codec_->flags |= AV_CODEC_FLAG_LOW_DELAY | AV_CODEC_FLAG_GLOBAL_HEADER;

        const AVSampleFormat* formats = nullptr;
        if (avcodec_get_supported_config(codec_, nullptr, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                         reinterpret_cast<const void**>(&formats), nullptr) >= 0 && formats)
            codec_->sample_fmt = formats[0];
        else
            codec_->sample_fmt = AV_SAMPLE_FMT_FLTP;

        if (avcodec_open2(codec_, encoder, nullptr) < 0 || codec_->frame_size <= 0) {
            error_ = {PlatformComponent::AudioCapture, PlatformFailure::Unavailable,
                      "could not open persistent AAC encoder", false};
            reset_encoder();
            return false;
        }

        packet_ = av_packet_alloc();
        encode_frame_ = av_frame_alloc();
        if (!packet_ || !encode_frame_) {
            reset_encoder();
            return false;
        }
        encode_frame_->nb_samples = codec_->frame_size;
        encode_frame_->format = codec_->sample_fmt;
        encode_frame_->sample_rate = codec_->sample_rate;
        if (av_channel_layout_copy(&encode_frame_->ch_layout, &codec_->ch_layout) < 0 ||
            av_frame_get_buffer(encode_frame_, 0) < 0) {
            reset_encoder();
            return false;
        }

        const int fifo_capacity = std::max({8192, codec_->frame_size * 8,
                                            static_cast<int>(buffer_frame_count_) * 8});
        fifo_ = av_audio_fifo_alloc(codec_->sample_fmt, codec_->ch_layout.nb_channels, fifo_capacity);
        if (!fifo_) {
            reset_encoder();
            return false;
        }

        AVChannelLayout input_layout{};
        av_channel_layout_default(&input_layout, input_channels_);
        if (swr_alloc_set_opts2(&swr_, &codec_->ch_layout, codec_->sample_fmt, input_rate_,
                                &input_layout, input_format_, input_rate_, 0, nullptr) < 0 ||
            !swr_ || swr_init(swr_) < 0) {
            av_channel_layout_uninit(&input_layout);
            error_ = {PlatformComponent::AudioCapture, PlatformFailure::Unavailable,
                      "could not initialize WASAPI audio resampler", false};
            reset_encoder();
            return false;
        }
        av_channel_layout_uninit(&input_layout);

        const int initial_resample_capacity = static_cast<int>(av_rescale_rnd(
            static_cast<std::int64_t>(buffer_frame_count_) + codec_->frame_size,
            codec_->sample_rate, input_rate_, AV_ROUND_UP));
        if (!allocate_resample_buffer(std::max(initial_resample_capacity, codec_->frame_size * 2))) {
            error_ = {PlatformComponent::AudioCapture, PlatformFailure::Unavailable,
                      "could not allocate persistent WASAPI resample buffer", false};
            reset_encoder();
            return false;
        }

        config_ = {};
        config_.kind = MediaKind::AudioAac;
        config_.sample_rate = codec_->sample_rate;
        config_.channels = codec_->ch_layout.nb_channels;
        if (codec_->extradata && codec_->extradata_size > 0)
            config_.extradata.assign(codec_->extradata, codec_->extradata + codec_->extradata_size);
        config_revision_ = 1;
        return true;
    }

    bool drain_capture_packets()
    {
        for (;;) {
            UINT32 packet_frames = 0;
            HRESULT hr = capture_client_->GetNextPacketSize(&packet_frames);
            if (FAILED(hr)) {
                error_ = audio_error(hr, "WASAPI GetNextPacketSize failed", true);
                running_ = false;
                return false;
            }
            if (packet_frames == 0) return true;

            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 device_position = 0;
            UINT64 qpc_position = 0;
            hr = capture_client_->GetBuffer(&data, &frames, &flags, &device_position, &qpc_position);
            if (FAILED(hr)) {
                error_ = audio_error(hr, "WASAPI GetBuffer failed", true);
                running_ = false;
                return false;
            }

            const std::uint64_t callback_us = monotonic_us();
            const std::uint64_t capture_us = qpc_capture_time_us(qpc_position, callback_us);
            bool ok = true;
            if (frames > 0) {
                if (frames > buffer_frame_count_) {
                    error_ = {PlatformComponent::AudioCapture, PlatformFailure::InvalidState,
                              "WASAPI returned a packet larger than its advertised shared buffer", true};
                    ok = false;
                } else if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !data) {
                    const auto bytes = static_cast<std::size_t>(frames) * static_cast<std::size_t>(input_block_align_);
                    std::fill_n(silence_.data(), bytes, std::uint8_t{0});
                    ok = ingest_pcm(silence_.data(), static_cast<int>(frames), capture_us);
                } else {
                    ok = ingest_pcm(data, static_cast<int>(frames), capture_us);
                }
            }
            const HRESULT release_hr = capture_client_->ReleaseBuffer(frames);
            if (!ok) {
                running_ = false;
                return false;
            }
            if (FAILED(release_hr)) {
                error_ = audio_error(release_hr, "WASAPI ReleaseBuffer failed", true);
                running_ = false;
                return false;
            }
        }
    }

    bool ingest_pcm(const std::uint8_t* data, int frames, std::uint64_t capture_us)
    {
        if (!data || frames <= 0 || !swr_ || !fifo_ || !resample_data_) return false;
        const int capacity = static_cast<int>(av_rescale_rnd(
            swr_get_delay(swr_, input_rate_) + frames,
            codec_->sample_rate, input_rate_, AV_ROUND_UP));
        if (capacity <= 0) return true;
        if (!ensure_resample_capacity(capacity)) {
            error_ = {PlatformComponent::AudioCapture, PlatformFailure::Unavailable,
                      "could not grow persistent WASAPI resample buffer", false};
            return false;
        }

        const std::uint8_t* input[1] = {data};
        const int produced = swr_convert(swr_, resample_data_, resample_capacity_, input, frames);
        if (produced < 0) {
            error_ = {PlatformComponent::AudioCapture, PlatformFailure::OsError,
                      "WASAPI audio resampling failed", false};
            return false;
        }
        if (produced == 0) return true;

        const bool fifo_was_empty = av_audio_fifo_size(fifo_) == 0;
        if (!ensure_fifo_space(produced) ||
            av_audio_fifo_write(fifo_, reinterpret_cast<void**>(resample_data_), produced) < produced) {
            error_ = {PlatformComponent::AudioCapture, PlatformFailure::Unavailable,
                      "could not queue resampled WASAPI audio", false};
            return false;
        }
        if (fifo_was_empty) fifo_capture_us_ = capture_us;
        return true;
    }

    bool encode_one(EncodedMediaUnit& unit)
    {
        if (!codec_ || !fifo_ || !encode_frame_ || av_audio_fifo_size(fifo_) < codec_->frame_size) return true;
        if (av_frame_make_writable(encode_frame_) < 0) {
            error_ = {PlatformComponent::AudioCapture, PlatformFailure::Unavailable,
                      "persistent AAC frame could not be made writable", false};
            return false;
        }
        if (av_audio_fifo_read(fifo_, reinterpret_cast<void**>(encode_frame_->data), codec_->frame_size) < codec_->frame_size)
            return false;

        encode_frame_->pts = static_cast<std::int64_t>(samples_submitted_);
        pending_capture_times_.push_back(fifo_capture_us_);
        fifo_capture_us_ += static_cast<std::uint64_t>(codec_->frame_size) * 1000000ULL /
                            static_cast<std::uint64_t>(codec_->sample_rate);
        samples_submitted_ += static_cast<std::uint64_t>(codec_->frame_size);

        const int send_rc = avcodec_send_frame(codec_, encode_frame_);
        if (send_rc < 0) {
            error_ = {PlatformComponent::AudioCapture, PlatformFailure::OsError,
                      "AAC encoder rejected WASAPI audio frame", false};
            return false;
        }

        av_packet_unref(packet_);
        const int receive_rc = avcodec_receive_packet(codec_, packet_);
        if (receive_rc == AVERROR(EAGAIN) || receive_rc == AVERROR_EOF) return true;
        if (receive_rc < 0) {
            error_ = {PlatformComponent::AudioCapture, PlatformFailure::OsError,
                      "AAC encoder failed to produce WASAPI packet", false};
            return false;
        }

        unit.kind = MediaKind::AudioAac;
        unit.data.assign(packet_->data, packet_->data + packet_->size);
        unit.pts_us = packet_->pts >= 0
            ? static_cast<std::int64_t>(packet_->pts * 1000000LL / codec_->sample_rate)
            : 0;
        if (!pending_capture_times_.empty()) {
            unit.capture_time_us = pending_capture_times_.front();
            pending_capture_times_.pop_front();
        }
        unit.keyframe = false;
        av_packet_unref(packet_);
        return !unit.data.empty();
    }

    void reset_encoder()
    {
        if (resample_data_) {
            av_freep(&resample_data_[0]);
            av_freep(&resample_data_);
        }
        resample_linesize_ = 0;
        resample_capacity_ = 0;
        if (encode_frame_) av_frame_free(&encode_frame_);
        if (fifo_) av_audio_fifo_free(fifo_);
        fifo_ = nullptr;
        if (swr_) swr_free(&swr_);
        if (packet_) av_packet_free(&packet_);
        if (codec_) avcodec_free_context(&codec_);
    }

    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* device_ = nullptr;
    IAudioClient* audio_client_ = nullptr;
    IAudioCaptureClient* capture_client_ = nullptr;
    WAVEFORMATEX* mix_format_ = nullptr;
    HANDLE event_ = nullptr;
    UINT32 buffer_frame_count_ = 0;
    bool com_initialized_ = false;
    bool started_client_ = false;
    bool running_ = false;

    AVCodecContext* codec_ = nullptr;
    SwrContext* swr_ = nullptr;
    AVAudioFifo* fifo_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* encode_frame_ = nullptr;
    std::uint8_t** resample_data_ = nullptr;
    int resample_linesize_ = 0;
    int resample_capacity_ = 0;
    AVSampleFormat input_format_ = AV_SAMPLE_FMT_NONE;
    int input_rate_ = 0;
    int input_channels_ = 0;
    int input_block_align_ = 0;
    std::uint64_t fifo_capture_us_ = 0;
    std::uint64_t samples_submitted_ = 0;
    std::deque<std::uint64_t> pending_capture_times_;
    std::vector<std::uint8_t> silence_;

    MediaConfig config_{};
    std::uint64_t config_revision_ = 0;
    PlatformError error_{};
};

std::unique_ptr<AudioCaptureBackend> make_audio_capture_backend()
{
    return std::make_unique<WindowsAudioCaptureBackend>();
}

}
