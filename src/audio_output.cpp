#include <opal/audio_output.hpp>

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace opal {
namespace {
constexpr std::uint32_t kAudioTargetQueueMs=24;
}

struct AudioOutput::Impl {
    AVCodecContext *decoder=nullptr;SwrContext *swr=nullptr;AVPacket *packet=nullptr;AVFrame *frame=nullptr;AVChannelLayout swr_input_layout{};bool swr_layout_valid=false;AVSampleFormat swr_input_format=AV_SAMPLE_FMT_NONE;int swr_input_rate=0;int sample_rate=0,channels=0;std::vector<std::uint8_t> packet_bytes,pcm_scratch;mutable std::mutex mutex;SDL_AudioStream*audio_stream=nullptr;bool audio_subsystem_owned=false;std::string test_sink;std::uint32_t test_queue_ms=0;
    ~Impl(){release_codec_state();}
    void release_resampler(){if(swr)swr_free(&swr);if(swr_layout_valid){av_channel_layout_uninit(&swr_input_layout);swr_layout_valid=false;}swr_input_format=AV_SAMPLE_FMT_NONE;swr_input_rate=0;}
    void release_codec_state(){release_resampler();if(packet)av_packet_free(&packet);if(frame)av_frame_free(&frame);if(decoder)avcodec_free_context(&decoder);packet_bytes.clear();pcm_scratch.clear();}
    bool ensure_resampler(AVFrame *input){if(!input||input->format<0)return false;AVChannelLayout in_layout{};if(input->ch_layout.nb_channels>0){if(av_channel_layout_copy(&in_layout,&input->ch_layout)<0)return false;}else av_channel_layout_default(&in_layout,channels);const auto input_format=static_cast<AVSampleFormat>(input->format);const int input_rate=input->sample_rate>0?input->sample_rate:sample_rate;const bool same=swr&&swr_layout_valid&&swr_input_format==input_format&&swr_input_rate==input_rate&&av_channel_layout_compare(&swr_input_layout,&in_layout)==0;if(same){av_channel_layout_uninit(&in_layout);return true;}release_resampler();if(av_channel_layout_copy(&swr_input_layout,&in_layout)<0){av_channel_layout_uninit(&in_layout);return false;}swr_layout_valid=true;swr_input_format=input_format;swr_input_rate=input_rate;AVChannelLayout out_layout{};av_channel_layout_default(&out_layout,channels);const int rc=swr_alloc_set_opts2(&swr,&out_layout,AV_SAMPLE_FMT_S16,sample_rate,&in_layout,input_format,input_rate,0,nullptr);av_channel_layout_uninit(&out_layout);av_channel_layout_uninit(&in_layout);if(rc<0||!swr||swr_init(swr)<0){release_resampler();return false;}return true;}
    std::uint32_t bytes_to_ms(int bytes)const{if(bytes<=0||sample_rate<=0||channels<=0)return 0;const std::uint64_t bytes_per_second=static_cast<std::uint64_t>(sample_rate)*static_cast<std::uint64_t>(channels)*2u;return static_cast<std::uint32_t>((static_cast<std::uint64_t>(bytes)*1000u+bytes_per_second-1u)/bytes_per_second);}
    bool open_audio(){if(!test_sink.empty())return true;if((SDL_WasInit(SDL_INIT_AUDIO)&SDL_INIT_AUDIO)==0){if(!SDL_InitSubSystem(SDL_INIT_AUDIO))return false;audio_subsystem_owned=true;}SDL_AudioSpec spec{};spec.format=SDL_AUDIO_S16LE;spec.channels=channels;spec.freq=sample_rate;audio_stream=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,nullptr,nullptr);if(!audio_stream)return false;if(!SDL_ResumeAudioStreamDevice(audio_stream)){SDL_DestroyAudioStream(audio_stream);audio_stream=nullptr;return false;}return true;}
    bool queue_pcm(const std::uint8_t*data,std::size_t bytes,std::uint32_t duration_ms){if(!data||bytes==0||duration_ms==0)return true;std::lock_guard<std::mutex>lock(mutex);if(test_sink=="discard")return true;if(test_sink=="hold"){if(test_queue_ms+duration_ms>kAudioTargetQueueMs)test_queue_ms=0;if(duration_ms<=kAudioTargetQueueMs)test_queue_ms+=duration_ms;return true;}if(!audio_stream)return false;const int queued=SDL_GetAudioStreamQueued(audio_stream);if(queued<0)return false;if(bytes_to_ms(queued)+duration_ms>kAudioTargetQueueMs){if(!SDL_ClearAudioStream(audio_stream))return false;}return SDL_PutAudioStreamData(audio_stream,data,static_cast<int>(bytes));}
    void clear_audio(){std::lock_guard<std::mutex>lock(mutex);test_queue_ms=0;if(audio_stream)(void)SDL_ClearAudioStream(audio_stream);}
};

AudioOutput::AudioOutput():impl_(std::make_unique<Impl>()){}
bool AudioOutput::configure_aac(std::span<const std::uint8_t> extradata,int sample_rate,int channels){close();impl_=std::make_unique<Impl>();if(sample_rate<=0||channels<=0||channels>32)return false;const AVCodec *codec=avcodec_find_decoder(AV_CODEC_ID_AAC);if(!codec)return false;impl_->decoder=avcodec_alloc_context3(codec);if(!impl_->decoder)return false;impl_->packet=av_packet_alloc();impl_->frame=av_frame_alloc();if(!impl_->packet||!impl_->frame){close();return false;}impl_->sample_rate=sample_rate;impl_->channels=channels;impl_->decoder->sample_rate=sample_rate;av_channel_layout_default(&impl_->decoder->ch_layout,channels);if(!extradata.empty()){impl_->decoder->extradata=static_cast<std::uint8_t*>(av_mallocz(extradata.size()+AV_INPUT_BUFFER_PADDING_SIZE));if(!impl_->decoder->extradata){close();return false;}std::memcpy(impl_->decoder->extradata,extradata.data(),extradata.size());impl_->decoder->extradata_size=static_cast<int>(extradata.size());}if(avcodec_open2(impl_->decoder,codec,nullptr)<0){close();return false;}if(const char*s=std::getenv("OPAL_AUDIO_TEST_SINK");s&&*s)impl_->test_sink=s;if(!impl_->open_audio()){close();return false;}return true;}

bool AudioOutput::submit(std::span<const std::uint8_t> aac,std::int64_t pts_us,std::int64_t current_video_pts_us){if(!impl_||!impl_->decoder||!impl_->packet||!impl_->frame||aac.empty())return false;if(current_video_pts_us>0&&pts_us+35000<current_video_pts_us)return true;const std::size_t needed=aac.size()+AV_INPUT_BUFFER_PADDING_SIZE;if(impl_->packet_bytes.size()<needed)impl_->packet_bytes.resize(needed);std::memcpy(impl_->packet_bytes.data(),aac.data(),aac.size());std::memset(impl_->packet_bytes.data()+aac.size(),0,AV_INPUT_BUFFER_PADDING_SIZE);av_packet_unref(impl_->packet);impl_->packet->data=impl_->packet_bytes.data();impl_->packet->size=static_cast<int>(aac.size());impl_->packet->pts=pts_us;impl_->packet->dts=pts_us;int rc=avcodec_send_packet(impl_->decoder,impl_->packet);impl_->packet->data=nullptr;impl_->packet->size=0;if(rc<0)return false;for(;;){av_frame_unref(impl_->frame);rc=avcodec_receive_frame(impl_->decoder,impl_->frame);if(rc==AVERROR(EAGAIN)||rc==AVERROR_EOF)break;if(rc<0)return false;if(!impl_->ensure_resampler(impl_->frame))return false;const int input_rate=impl_->frame->sample_rate>0?impl_->frame->sample_rate:impl_->sample_rate;const int capacity=static_cast<int>(av_rescale_rnd(swr_get_delay(impl_->swr,input_rate)+impl_->frame->nb_samples,impl_->sample_rate,input_rate,AV_ROUND_UP));if(capacity<=0)continue;const std::size_t capacity_bytes=static_cast<std::size_t>(capacity)*impl_->channels*2;impl_->pcm_scratch.resize(capacity_bytes);std::uint8_t*out_data[1]={impl_->pcm_scratch.data()};const auto **input=const_cast<const std::uint8_t **>(impl_->frame->extended_data);const int converted=swr_convert(impl_->swr,out_data,capacity,input,impl_->frame->nb_samples);if(converted<0)return false;const std::size_t pcm_bytes=static_cast<std::size_t>(converted)*impl_->channels*2;const auto duration=static_cast<std::uint32_t>((static_cast<std::uint64_t>(converted)*1000u+impl_->sample_rate-1)/impl_->sample_rate);if(!impl_->queue_pcm(impl_->pcm_scratch.data(),pcm_bytes,duration))return false;}av_frame_unref(impl_->frame);return true;}
void AudioOutput::reset_to(std::int64_t){if(impl_)impl_->clear_audio();}
std::uint32_t AudioOutput::queued_ms()const{if(!impl_)return 0;std::lock_guard<std::mutex>lock(impl_->mutex);if(impl_->test_sink=="hold")return impl_->test_queue_ms;if(!impl_->audio_stream)return 0;return impl_->bytes_to_ms(SDL_GetAudioStreamQueued(impl_->audio_stream));}
std::string AudioOutput::backend_name()const{return"sdl3";}
void AudioOutput::close(){if(!impl_)return;{std::lock_guard<std::mutex>lock(impl_->mutex);impl_->test_queue_ms=0;if(impl_->audio_stream){SDL_DestroyAudioStream(impl_->audio_stream);impl_->audio_stream=nullptr;}}if(impl_->audio_subsystem_owned){SDL_QuitSubSystem(SDL_INIT_AUDIO);impl_->audio_subsystem_owned=false;}impl_->release_codec_state();}
AudioOutput::~AudioOutput(){close();}
}
