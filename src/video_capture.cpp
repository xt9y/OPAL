#include <opal/video_capture.hpp>
#include <opal/config.hpp>
#include <opal/media.hpp>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mem.h>
}
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <utility>

namespace opal {
struct VideoCapture::Impl {
    CaptureProcess capture;
    AVFormatContext *format=nullptr;
    AVIOContext *avio=nullptr;
    int video_stream=-1;
    int audio_stream=-1;
    int read_timeout_ms=5000;
    bool terminal=false;
    bool timeline_anchored=false;
    std::int64_t timeline_origin_pts_us=0;
    std::uint64_t timeline_origin_monotonic_us=0;
    std::vector<MediaConfig> configs;

    static int read(void *opaque,std::uint8_t *buffer,int size){
        auto *impl=static_cast<Impl*>(opaque);
        if(!impl||size<=0)return AVERROR(EINVAL);
        int n=read_capture(impl->capture,buffer,static_cast<size_t>(size),impl->read_timeout_ms);
        if(n>0)return n;
        if(n==0)return AVERROR_EOF;
        if(n==-2)return AVERROR(EAGAIN);
        return AVERROR(EIO);
    }
};

namespace {
std::vector<std::uint8_t> copy_extra(const AVCodecParameters *par){
    if(!par||!par->extradata||par->extradata_size<=0)return {};
    return {par->extradata,par->extradata+par->extradata_size};
}
std::uint64_t monotonic_us(){
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
}

VideoCapture::VideoCapture():impl_(std::make_unique<Impl>()){}
VideoCapture::~VideoCapture(){stop();}

bool VideoCapture::start(const StreamOptions &stream,int bitrate_kbps,bool audio,
                         const std::string &portal_token_file){
    stop();
    if(!impl_)impl_=std::make_unique<Impl>();
    impl_->terminal=false;
    impl_->timeline_anchored=false;
    impl_->timeline_origin_pts_us=0;
    impl_->timeline_origin_monotonic_us=0;

    std::string command;
    if(const char *override_command=std::getenv("OPAL_CAPTURE_CMD");override_command&&*override_command)
        command=override_command;
    else
        command=capture_command(command_exists("gpu-screen-recorder"),stream.fps,bitrate_kbps,
                                audio,portal_token_file,stream.max_width,stream.max_height);

    impl_->capture=start_capture(command);
    if(impl_->capture.pid<=0||impl_->capture.fd<0){stop();return false;}

    auto *buffer=static_cast<unsigned char*>(av_malloc(64*1024));
    if(!buffer){stop();return false;}
    impl_->avio=avio_alloc_context(buffer,64*1024,0,impl_.get(),Impl::read,nullptr,nullptr);
    if(!impl_->avio){av_free(buffer);stop();return false;}

    impl_->format=avformat_alloc_context();
    if(!impl_->format){stop();return false;}
    impl_->format->pb=impl_->avio;
    impl_->format->flags|=AVFMT_FLAG_CUSTOM_IO;
    impl_->read_timeout_ms=5000;

    const AVInputFormat *flv=av_find_input_format("flv");
    AVFormatContext *opened=impl_->format;
    if(!flv||avformat_open_input(&opened,nullptr,flv,nullptr)<0){
        impl_->format=opened;
        stop();return false;
    }
    impl_->format=opened;
    if(avformat_find_stream_info(impl_->format,nullptr)<0){stop();return false;}

    impl_->configs.clear();
    for(unsigned int i=0;i<impl_->format->nb_streams;++i){
        AVStream *stream_info=impl_->format->streams[i];
        AVCodecParameters *par=stream_info?stream_info->codecpar:nullptr;
        if(!par)continue;
        if(par->codec_type==AVMEDIA_TYPE_VIDEO&&par->codec_id==AV_CODEC_ID_H264&&impl_->video_stream<0){
            impl_->video_stream=static_cast<int>(i);
            MediaConfig config;
            config.kind=MediaKind::VideoH264;
            config.extradata=copy_extra(par);
            impl_->configs.push_back(std::move(config));
        }else if(par->codec_type==AVMEDIA_TYPE_AUDIO&&par->codec_id==AV_CODEC_ID_AAC&&impl_->audio_stream<0){
            impl_->audio_stream=static_cast<int>(i);
            MediaConfig config;
            config.kind=MediaKind::AudioAac;
            config.extradata=copy_extra(par);
            config.sample_rate=par->sample_rate;
            config.channels=par->ch_layout.nb_channels;
            impl_->configs.push_back(std::move(config));
        }
    }

    if(impl_->video_stream<0){stop();return false;}
    if(audio&&impl_->audio_stream<0){stop();return false;}
    return true;
}

bool VideoCapture::next(EncodedMediaUnit &unit,int timeout_ms){
    if(!impl_||!impl_->format)return false;
    impl_->read_timeout_ms=std::max(1,timeout_ms);
    AVPacket *packet=av_packet_alloc();
    if(!packet)return false;

    bool produced=false;
    for(;;){
        int rc=av_read_frame(impl_->format,packet);
        if(rc<0){
            if(rc!=AVERROR(EAGAIN))impl_->terminal=true;
            break;
        }
        int stream_index=packet->stream_index;
        if(stream_index!=impl_->video_stream&&stream_index!=impl_->audio_stream){
            av_packet_unref(packet);
            continue;
        }

        AVStream *stream_info=impl_->format->streams[stream_index];
        unit={};
        unit.kind=stream_index==impl_->video_stream?MediaKind::VideoH264:MediaKind::AudioAac;
        if(packet->data&&packet->size>0)
            unit.data.assign(packet->data,packet->data+packet->size);
        std::int64_t timestamp=packet->pts!=AV_NOPTS_VALUE?packet->pts:packet->dts;
        if(timestamp!=AV_NOPTS_VALUE&&stream_info)
            unit.pts_us=av_rescale_q(timestamp,stream_info->time_base,AVRational{1,1000000});
        const auto now=monotonic_us();
        if(timestamp!=AV_NOPTS_VALUE&&stream_info){
            if(!impl_->timeline_anchored){
                impl_->timeline_anchored=true;
                impl_->timeline_origin_pts_us=unit.pts_us;
                impl_->timeline_origin_monotonic_us=now;
            }
            const auto delta=unit.pts_us-impl_->timeline_origin_pts_us;
            unit.capture_time_us=delta>0?impl_->timeline_origin_monotonic_us+static_cast<std::uint64_t>(delta):impl_->timeline_origin_monotonic_us;
        }else{
            unit.capture_time_us=now;
        }
        unit.keyframe=unit.kind==MediaKind::VideoH264&&(packet->flags&AV_PKT_FLAG_KEY)!=0;
        produced=!unit.data.empty();
        av_packet_unref(packet);
        break;
    }

    av_packet_free(&packet);
    return produced;
}

bool VideoCapture::ended() const{return impl_&&impl_->terminal;}

const std::vector<MediaConfig>& VideoCapture::configs() const{
    static const std::vector<MediaConfig> empty;
    return impl_?impl_->configs:empty;
}

void VideoCapture::stop(){
    if(!impl_)return;
    if(impl_->format){
        avformat_close_input(&impl_->format);
        impl_->format=nullptr;
    }
    if(impl_->avio){
        av_freep(&impl_->avio->buffer);
        avio_context_free(&impl_->avio);
        impl_->avio=nullptr;
    }
    stop_capture(impl_->capture);
    impl_->video_stream=-1;
    impl_->audio_stream=-1;
    impl_->terminal=false;
    impl_->timeline_anchored=false;
    impl_->timeline_origin_pts_us=0;
    impl_->timeline_origin_monotonic_us=0;
    impl_->configs.clear();
}
}
