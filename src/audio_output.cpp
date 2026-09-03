#include <opal/audio_output.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <pulse/error.h>
#include <pulse/simple.h>
}

namespace opal {

struct AudioOutput::Impl {
    struct Chunk {std::vector<std::uint8_t> pcm;std::uint32_t ms=0;std::int64_t pts_us=0;};
    AVCodecContext *decoder=nullptr;
    SwrContext *swr=nullptr;
    AVPacket *packet=nullptr;
    AVFrame *frame=nullptr;
    AVChannelLayout swr_input_layout{};
    bool swr_layout_valid=false;
    AVSampleFormat swr_input_format=AV_SAMPLE_FMT_NONE;
    int swr_input_rate=0;
    int sample_rate=0,channels=0;
    std::vector<std::uint8_t> pcm_scratch;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<Chunk> queue;
    std::uint32_t queue_ms=0;
    bool run=false;
    std::thread thread;
    std::string test_sink;

    ~Impl(){release_codec_state();}

    void clear_locked(){queue.clear();queue_ms=0;}

    void release_resampler(){
        if(swr)swr_free(&swr);
        if(swr_layout_valid){av_channel_layout_uninit(&swr_input_layout);swr_layout_valid=false;}
        swr_input_format=AV_SAMPLE_FMT_NONE;swr_input_rate=0;
    }

    void release_codec_state(){
        release_resampler();
        if(packet)av_packet_free(&packet);
        if(frame)av_frame_free(&frame);
        if(decoder)avcodec_free_context(&decoder);
        pcm_scratch.clear();
    }

    bool ensure_resampler(AVFrame *input){
        if(!input||input->format<0)return false;
        AVChannelLayout in_layout{};
        if(input->ch_layout.nb_channels>0){
            if(av_channel_layout_copy(&in_layout,&input->ch_layout)<0)return false;
        }else av_channel_layout_default(&in_layout,channels);
        const auto input_format=static_cast<AVSampleFormat>(input->format);
        const int input_rate=input->sample_rate>0?input->sample_rate:sample_rate;
        const bool same=swr&&swr_layout_valid&&swr_input_format==input_format&&swr_input_rate==input_rate&&
                        av_channel_layout_compare(&swr_input_layout,&in_layout)==0;
        if(same){av_channel_layout_uninit(&in_layout);return true;}

        release_resampler();
        if(av_channel_layout_copy(&swr_input_layout,&in_layout)<0){av_channel_layout_uninit(&in_layout);return false;}
        swr_layout_valid=true;swr_input_format=input_format;swr_input_rate=input_rate;
        AVChannelLayout out_layout{};av_channel_layout_default(&out_layout,channels);
        const int alloc_rc=swr_alloc_set_opts2(&swr,&out_layout,AV_SAMPLE_FMT_S16,sample_rate,
                                                &in_layout,input_format,input_rate,0,nullptr);
        av_channel_layout_uninit(&out_layout);av_channel_layout_uninit(&in_layout);
        if(alloc_rc<0||!swr||swr_init(swr)<0){release_resampler();return false;}
        return true;
    }

    void enqueue(Chunk chunk){
        if(chunk.pcm.empty()||chunk.ms==0)return;
        if(chunk.ms>40){
            const std::size_t bytes_per_ms=std::max<std::size_t>(1,static_cast<std::size_t>(sample_rate)*channels*2/1000);
            const std::size_t keep=std::min(chunk.pcm.size(),bytes_per_ms*40);
            std::vector<std::uint8_t> tail(chunk.pcm.end()-static_cast<std::ptrdiff_t>(keep),chunk.pcm.end());
            chunk.pcm=std::move(tail);chunk.ms=40;
        }
        std::lock_guard<std::mutex> lock(mutex);
        while(!queue.empty()&&queue_ms+chunk.ms>40){queue_ms-=queue.front().ms;queue.pop_front();}
        if(queue_ms+chunk.ms>40)return;
        queue_ms+=chunk.ms;queue.push_back(std::move(chunk));cv.notify_one();
    }

    void playback(){
        pa_simple *pulse=nullptr;
        if(test_sink.empty()){
            pa_sample_spec spec{};spec.format=PA_SAMPLE_S16LE;spec.rate=static_cast<std::uint32_t>(sample_rate);spec.channels=static_cast<std::uint8_t>(channels);
            const std::uint32_t bytes_per_ms=std::max(1u,static_cast<std::uint32_t>(sample_rate*channels*2/1000));
            pa_buffer_attr attr{};attr.maxlength=bytes_per_ms*40;attr.tlength=bytes_per_ms*40;attr.prebuf=0;attr.minreq=bytes_per_ms*10;attr.fragsize=static_cast<std::uint32_t>(-1);
            int error=0;pulse=pa_simple_new(nullptr,"OPAL",PA_STREAM_PLAYBACK,nullptr,"Remote audio",&spec,nullptr,&attr,&error);
        }
        for(;;){
            Chunk chunk;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock,[&]{return !run||!queue.empty();});
                if(!run){clear_locked();break;}
                if(test_sink=="hold"){lock.unlock();std::this_thread::sleep_for(std::chrono::milliseconds(5));continue;}
                chunk=std::move(queue.front());queue.pop_front();queue_ms-=chunk.ms;
            }
            if(test_sink=="discard"||!pulse)continue;
            int error=0;if(pa_simple_write(pulse,chunk.pcm.data(),chunk.pcm.size(),&error)<0){pa_simple_free(pulse);pulse=nullptr;}
        }
        if(pulse){int error=0;pa_simple_flush(pulse,&error);pa_simple_free(pulse);}
    }
};

AudioOutput::AudioOutput():impl_(std::make_unique<Impl>()){}

bool AudioOutput::configure_aac(std::span<const std::uint8_t> extradata,int sample_rate,int channels){
    close();impl_=std::make_unique<Impl>();
    if(sample_rate<=0||channels<=0||channels>32)return false;
    const AVCodec *codec=avcodec_find_decoder(AV_CODEC_ID_AAC);if(!codec)return false;
    impl_->decoder=avcodec_alloc_context3(codec);if(!impl_->decoder)return false;
    impl_->packet=av_packet_alloc();impl_->frame=av_frame_alloc();
    if(!impl_->packet||!impl_->frame){close();return false;}
    impl_->sample_rate=sample_rate;impl_->channels=channels;impl_->decoder->sample_rate=sample_rate;
    av_channel_layout_default(&impl_->decoder->ch_layout,channels);
    if(!extradata.empty()){
        impl_->decoder->extradata=static_cast<std::uint8_t*>(av_mallocz(extradata.size()+AV_INPUT_BUFFER_PADDING_SIZE));
        if(!impl_->decoder->extradata){close();return false;}
        std::memcpy(impl_->decoder->extradata,extradata.data(),extradata.size());impl_->decoder->extradata_size=static_cast<int>(extradata.size());
    }
    if(avcodec_open2(impl_->decoder,codec,nullptr)<0){close();return false;}
    if(const char *sink=std::getenv("OPAL_AUDIO_TEST_SINK");sink&&*sink)impl_->test_sink=sink;
    {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->run=true;}
    impl_->thread=std::thread([this]{impl_->playback();});return true;
}

bool AudioOutput::submit(std::span<const std::uint8_t> aac,std::int64_t pts_us,std::int64_t current_video_pts_us){
    if(!impl_||!impl_->decoder||!impl_->packet||!impl_->frame||aac.empty())return false;
    if(current_video_pts_us>0&&pts_us+40000<current_video_pts_us)return true;
    av_packet_unref(impl_->packet);
    if(av_new_packet(impl_->packet,static_cast<int>(aac.size()))<0)return false;
    std::memcpy(impl_->packet->data,aac.data(),aac.size());impl_->packet->pts=pts_us;impl_->packet->dts=pts_us;
    int rc=avcodec_send_packet(impl_->decoder,impl_->packet);av_packet_unref(impl_->packet);if(rc<0)return false;
    for(;;){
        av_frame_unref(impl_->frame);
        rc=avcodec_receive_frame(impl_->decoder,impl_->frame);
        if(rc==AVERROR(EAGAIN)||rc==AVERROR_EOF)break;
        if(rc<0)return false;
        if(!impl_->ensure_resampler(impl_->frame))return false;
        const int input_rate=impl_->frame->sample_rate>0?impl_->frame->sample_rate:impl_->sample_rate;
        const int capacity=static_cast<int>(av_rescale_rnd(swr_get_delay(impl_->swr,input_rate)+impl_->frame->nb_samples,
                                                            impl_->sample_rate,input_rate,AV_ROUND_UP));
        if(capacity<=0)continue;
        const std::size_t capacity_bytes=static_cast<std::size_t>(capacity)*impl_->channels*2;
        impl_->pcm_scratch.resize(capacity_bytes);
        std::uint8_t *out_data[1]={impl_->pcm_scratch.data()};
        const auto **input=const_cast<const std::uint8_t **>(impl_->frame->extended_data);
        const int converted=swr_convert(impl_->swr,out_data,capacity,input,impl_->frame->nb_samples);
        if(converted<0)return false;
        const std::size_t pcm_bytes=static_cast<std::size_t>(converted)*impl_->channels*2;
        const auto duration=static_cast<std::uint32_t>((static_cast<std::uint64_t>(converted)*1000u+impl_->sample_rate-1)/impl_->sample_rate);
        AudioOutput::Impl::Chunk chunk;chunk.pcm.assign(impl_->pcm_scratch.begin(),impl_->pcm_scratch.begin()+static_cast<std::ptrdiff_t>(pcm_bytes));chunk.ms=duration;chunk.pts_us=pts_us;
        impl_->enqueue(std::move(chunk));
    }
    av_frame_unref(impl_->frame);
    return true;
}

void AudioOutput::reset_to(std::int64_t){if(impl_){std::lock_guard<std::mutex> lock(impl_->mutex);impl_->clear_locked();}}
std::uint32_t AudioOutput::queued_ms() const{if(!impl_)return 0;std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->queue_ms;}

void AudioOutput::close(){
    if(!impl_)return;
    {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->run=false;impl_->cv.notify_all();}
    if(impl_->thread.joinable())impl_->thread.join();
    {std::lock_guard<std::mutex> lock(impl_->mutex);impl_->clear_locked();}
    impl_->release_codec_state();
}

AudioOutput::~AudioOutput(){close();}

}
