#include <opal/video_capture.hpp>
#include <opal/config.hpp>
#include <opal/media.hpp>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <string>
#include <utility>

namespace opal {
namespace {
std::vector<std::uint8_t> copy_extra(const AVCodecParameters *par){if(!par||!par->extradata||par->extradata_size<=0)return{};return{par->extradata,par->extradata+par->extradata_size};}
std::uint64_t monotonic_us(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
bool debug_enabled(){const char*v=std::getenv("OPAL_DEBUG");return v&&*v&&std::string(v)!="0";}
std::string av_error_text(int rc){char buffer[AV_ERROR_MAX_STRING_SIZE]{};return av_strerror(rc,buffer,sizeof(buffer))==0?std::string(buffer):std::string("ffmpeg error ")+std::to_string(rc);}
}

struct VideoCapture::Impl {
    CaptureProcess capture;
    AVFormatContext *format=nullptr;
    AVIOContext *avio=nullptr;
    AVPacket *packet=nullptr;
    int video_stream=-1,audio_stream=-1,read_timeout_ms=2000;
    bool terminal=false,timeline_anchored=false;
    std::int64_t timeline_origin_pts_us=0;
    std::uint64_t timeline_origin_monotonic_us=0;
    std::vector<MediaConfig> configs;
    std::deque<AVPacket*> prefetched;
    std::string backend="unconfigured",error;
    bool timestamp_estimated=true;

    ~Impl(){clear_prefetched();if(packet)av_packet_free(&packet);}

    void clear_prefetched(){while(!prefetched.empty()){AVPacket *p=prefetched.front();prefetched.pop_front();av_packet_free(&p);}}

    bool refresh_configs(bool require_audio){
        int next_video=-1,next_audio=-1;
        bool video_ready=false,audio_ready=!require_audio;
        std::vector<MediaConfig> next_configs;
        if(!format)return false;
        for(unsigned int i=0;i<format->nb_streams;++i){
            AVStream *stream=format->streams[i];
            AVCodecParameters *par=stream?stream->codecpar:nullptr;
            if(!par)continue;
            if(par->codec_type==AVMEDIA_TYPE_VIDEO&&par->codec_id==AV_CODEC_ID_H264&&next_video<0){
                next_video=static_cast<int>(i);
                MediaConfig config;config.kind=MediaKind::VideoH264;config.extradata=copy_extra(par);
                video_ready=!config.extradata.empty();next_configs.push_back(std::move(config));
            }else if(par->codec_type==AVMEDIA_TYPE_AUDIO&&par->codec_id==AV_CODEC_ID_AAC&&next_audio<0){
                next_audio=static_cast<int>(i);
                MediaConfig config;config.kind=MediaKind::AudioAac;config.extradata=copy_extra(par);config.sample_rate=par->sample_rate;config.channels=par->ch_layout.nb_channels;
                audio_ready=!require_audio||(!config.extradata.empty()&&config.sample_rate>0&&config.channels>0);next_configs.push_back(std::move(config));
            }
        }
        video_stream=next_video;audio_stream=next_audio;configs=std::move(next_configs);
        return video_ready&&audio_ready;
    }

    bool queue_prefetched(const AVPacket *source){
        if(!source||source->size<=0||source->stream_index<0||!format||static_cast<unsigned int>(source->stream_index)>=format->nb_streams)return true;
        AVStream *stream=format->streams[source->stream_index];AVCodecParameters *par=stream?stream->codecpar:nullptr;
        if(!par||(par->codec_type!=AVMEDIA_TYPE_VIDEO&&par->codec_type!=AVMEDIA_TYPE_AUDIO))return true;
        constexpr std::size_t kMaxStartupPackets=16;
        if(prefetched.size()>=kMaxStartupPackets)return true;
        AVPacket *copy=av_packet_clone(source);if(!copy)return false;prefetched.push_back(copy);return true;
    }

    static int read(void *opaque,std::uint8_t *buffer,int size){
        auto *impl=static_cast<Impl*>(opaque);if(!impl||size<=0)return AVERROR(EINVAL);
        const int n=read_capture(impl->capture,buffer,static_cast<size_t>(size),impl->read_timeout_ms);
        if(n>0)return n;if(n==0)return AVERROR_EOF;if(n==-2)return AVERROR(EAGAIN);return AVERROR(EIO);
    }
};

VideoCapture::VideoCapture():impl_(std::make_unique<Impl>()){}
VideoCapture::~VideoCapture(){stop();}

bool VideoCapture::start(const StreamOptions &stream,int bitrate_kbps,bool audio,const std::string &portal_token_file){
    stop();if(!impl_)impl_=std::make_unique<Impl>();
    impl_->error.clear();impl_->terminal=false;impl_->timeline_anchored=false;impl_->timeline_origin_pts_us=0;impl_->timeline_origin_monotonic_us=0;impl_->timestamp_estimated=true;impl_->clear_prefetched();
    auto fail=[&](std::string message){impl_->error=std::move(message);if(debug_enabled())std::cerr<<"OPAL capture startup failed backend="<<impl_->backend<<" reason="<<impl_->error<<"\n";stop();return false;};
    if(!impl_->packet)impl_->packet=av_packet_alloc();if(!impl_->packet)return fail("packet allocation failed");

    std::string command;
    if(const char*override_command=std::getenv("OPAL_CAPTURE_CMD");override_command&&*override_command){command=override_command;impl_->backend="external-override-flv";}
    else{const bool gsr=command_exists("gpu-screen-recorder");command=capture_command(gsr,stream.fps,bitrate_kbps,audio,portal_token_file,stream.max_width,stream.max_height);impl_->backend=gsr?"gpu-screen-recorder-flv":"ffmpeg-x11grab-flv";}
    if(command.empty())return fail("no usable capture command");

    impl_->capture=start_capture(command);if(impl_->capture.pid<=0||impl_->capture.fd<0)return fail("capture process did not start");
    constexpr int avio_bytes=8*1024;auto *buffer=static_cast<unsigned char*>(av_malloc(avio_bytes));if(!buffer)return fail("AVIO buffer allocation failed");
    impl_->avio=avio_alloc_context(buffer,avio_bytes,0,impl_.get(),Impl::read,nullptr,nullptr);if(!impl_->avio){av_free(buffer);return fail("AVIO context allocation failed");}
    impl_->format=avformat_alloc_context();if(!impl_->format)return fail("format context allocation failed");
    impl_->format->pb=impl_->avio;impl_->format->flags|=AVFMT_FLAG_CUSTOM_IO;impl_->read_timeout_ms=2000;
    const AVInputFormat *flv=av_find_input_format("flv");if(!flv)return fail("FLV demuxer unavailable");
    AVFormatContext *opened=impl_->format;const int open_rc=avformat_open_input(&opened,nullptr,flv,nullptr);impl_->format=opened;if(open_rc<0)return fail("FLV open failed: "+av_error_text(open_rc));

    bool ready=impl_->refresh_configs(audio);
    AVPacket *probe=av_packet_alloc();if(!probe)return fail("startup packet allocation failed");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(2500);
    impl_->read_timeout_ms=250;
    int last_rc=0,packets=0;
    while(!ready&&packets<64&&std::chrono::steady_clock::now()<deadline){
        av_packet_unref(probe);last_rc=av_read_frame(impl_->format,probe);
        if(last_rc==AVERROR(EAGAIN))continue;
        if(last_rc<0)break;
        ++packets;
        if(!impl_->queue_prefetched(probe)){av_packet_free(&probe);return fail("startup packet prefetch allocation failed");}
        ready=impl_->refresh_configs(audio);
    }
    av_packet_free(&probe);
    if(!ready){
        if(last_rc<0&&last_rc!=AVERROR(EAGAIN))return fail("FLV stream discovery failed: "+av_error_text(last_rc));
        if(impl_->video_stream<0)return fail("FLV stream has no H.264 video track");
        bool video_config=false,audio_config=!audio;
        for(const auto&config:impl_->configs){if(config.kind==MediaKind::VideoH264&&!config.extradata.empty())video_config=true;if(config.kind==MediaKind::AudioAac&&!config.extradata.empty()&&config.sample_rate>0&&config.channels>0)audio_config=true;}
        if(!video_config)return fail("FLV H.264 configuration was not received before startup deadline");
        if(!audio_config)return fail("FLV AAC configuration was not received before startup deadline");
        return fail("FLV stream discovery timed out");
    }

    impl_->format->flags|=AVFMT_FLAG_NOBUFFER;impl_->read_timeout_ms=5000;
    if(debug_enabled())std::cerr<<"OPAL capture backend="<<impl_->backend<<" acquisition_timestamp="<<(impl_->timestamp_estimated?"estimated":"native")<<" startup_prefetch="<<impl_->prefetched.size()<<"\n";
    return true;
}

bool VideoCapture::next_view(EncodedMediaView &unit,int timeout_ms){
    if(!impl_||!impl_->format||!impl_->packet)return false;
    impl_->read_timeout_ms=std::max(1,timeout_ms);unit.kind=MediaKind::VideoH264;unit.data={};unit.pts_us=0;unit.capture_time_us=0;unit.keyframe=false;
    for(;;){
        av_packet_unref(impl_->packet);int rc=0;
        if(!impl_->prefetched.empty()){
            AVPacket *queued=impl_->prefetched.front();impl_->prefetched.pop_front();av_packet_move_ref(impl_->packet,queued);av_packet_free(&queued);
        }else rc=av_read_frame(impl_->format,impl_->packet);
        if(rc<0){if(rc!=AVERROR(EAGAIN))impl_->terminal=true;return false;}
        const int stream_index=impl_->packet->stream_index;if(stream_index!=impl_->video_stream&&stream_index!=impl_->audio_stream)continue;
        AVStream *stream=impl_->format->streams[stream_index];unit.kind=stream_index==impl_->video_stream?MediaKind::VideoH264:MediaKind::AudioAac;
        if(impl_->packet->data&&impl_->packet->size>0)unit.data=std::span<const std::uint8_t>(impl_->packet->data,static_cast<std::size_t>(impl_->packet->size));
        const std::int64_t timestamp=impl_->packet->pts!=AV_NOPTS_VALUE?impl_->packet->pts:impl_->packet->dts;
        if(timestamp!=AV_NOPTS_VALUE&&stream)unit.pts_us=av_rescale_q(timestamp,stream->time_base,AVRational{1,1000000});
        const auto now=monotonic_us();
        if(timestamp!=AV_NOPTS_VALUE&&stream){if(!impl_->timeline_anchored){impl_->timeline_anchored=true;impl_->timeline_origin_pts_us=unit.pts_us;impl_->timeline_origin_monotonic_us=now;}const auto delta=unit.pts_us-impl_->timeline_origin_pts_us;unit.capture_time_us=delta>0?impl_->timeline_origin_monotonic_us+static_cast<std::uint64_t>(delta):impl_->timeline_origin_monotonic_us;}else unit.capture_time_us=now;
        unit.keyframe=unit.kind==MediaKind::VideoH264&&(impl_->packet->flags&AV_PKT_FLAG_KEY)!=0;return !unit.data.empty();
    }
}

bool VideoCapture::next(EncodedMediaUnit &unit,int timeout_ms){EncodedMediaView view;if(!next_view(view,timeout_ms))return false;unit.kind=view.kind;unit.data.clear();unit.data.assign(view.data.begin(),view.data.end());unit.pts_us=view.pts_us;unit.capture_time_us=view.capture_time_us;unit.keyframe=view.keyframe;return !unit.data.empty();}
bool VideoCapture::ended()const{return impl_&&impl_->terminal;}
const std::vector<MediaConfig>&VideoCapture::configs()const{static const std::vector<MediaConfig>empty;return impl_?impl_->configs:empty;}
std::string VideoCapture::backend_name()const{return impl_?impl_->backend:"unavailable";}
std::string VideoCapture::last_error()const{return impl_?impl_->error:"capture unavailable";}
bool VideoCapture::capture_timestamp_estimated()const{return !impl_||impl_->timestamp_estimated;}

void VideoCapture::stop(){
    if(!impl_)return;
    impl_->clear_prefetched();if(impl_->packet)av_packet_unref(impl_->packet);
    if(impl_->format){avformat_close_input(&impl_->format);impl_->format=nullptr;}
    if(impl_->avio){av_freep(&impl_->avio->buffer);avio_context_free(&impl_->avio);impl_->avio=nullptr;}
    stop_capture(impl_->capture);impl_->video_stream=-1;impl_->audio_stream=-1;impl_->terminal=false;impl_->timeline_anchored=false;impl_->timeline_origin_pts_us=0;impl_->timeline_origin_monotonic_us=0;impl_->configs.clear();impl_->backend="unconfigured";impl_->timestamp_estimated=true;
}
}
