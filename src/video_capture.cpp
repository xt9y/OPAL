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
struct VideoCapture::Impl {CaptureProcess capture;AVFormatContext *format=nullptr;AVIOContext *avio=nullptr;AVPacket *packet=nullptr;int video_stream=-1,audio_stream=-1,read_timeout_ms=5000;bool terminal=false,timeline_anchored=false;std::int64_t timeline_origin_pts_us=0;std::uint64_t timeline_origin_monotonic_us=0;std::vector<MediaConfig> configs;std::string backend="unconfigured";bool timestamp_estimated=true;~Impl(){if(packet)av_packet_free(&packet);}static int read(void *opaque,std::uint8_t *buffer,int size){auto *impl=static_cast<Impl*>(opaque);if(!impl||size<=0)return AVERROR(EINVAL);int n=read_capture(impl->capture,buffer,static_cast<size_t>(size),impl->read_timeout_ms);if(n>0)return n;if(n==0)return AVERROR_EOF;if(n==-2)return AVERROR(EAGAIN);return AVERROR(EIO);}};
namespace {std::vector<std::uint8_t> copy_extra(const AVCodecParameters *par){if(!par||!par->extradata||par->extradata_size<=0)return{};return{par->extradata,par->extradata+par->extradata_size};}std::uint64_t monotonic_us(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}}
VideoCapture::VideoCapture():impl_(std::make_unique<Impl>()){}VideoCapture::~VideoCapture(){stop();}
bool VideoCapture::start(const StreamOptions &stream,int bitrate_kbps,bool audio,const std::string &portal_token_file){stop();if(!impl_)impl_=std::make_unique<Impl>();impl_->terminal=false;impl_->timeline_anchored=false;impl_->timeline_origin_pts_us=0;impl_->timeline_origin_monotonic_us=0;impl_->timestamp_estimated=true;if(!impl_->packet)impl_->packet=av_packet_alloc();if(!impl_->packet)return false;std::string command;if(const char*override_command=std::getenv("OPAL_CAPTURE_CMD");override_command&&*override_command){command=override_command;impl_->backend="external-override-flv";}else{const bool gsr=command_exists("gpu-screen-recorder");command=capture_command(gsr,stream.fps,bitrate_kbps,audio,portal_token_file,stream.max_width,stream.max_height);impl_->backend=gsr?"gpu-screen-recorder-flv":"ffmpeg-x11grab-flv";}impl_->capture=start_capture(command);if(impl_->capture.pid<=0||impl_->capture.fd<0){stop();return false;}constexpr int avio_bytes=8*1024;auto *buffer=static_cast<unsigned char*>(av_malloc(avio_bytes));if(!buffer){stop();return false;}impl_->avio=avio_alloc_context(buffer,avio_bytes,0,impl_.get(),Impl::read,nullptr,nullptr);if(!impl_->avio){av_free(buffer);stop();return false;}impl_->format=avformat_alloc_context();if(!impl_->format){stop();return false;}impl_->format->pb=impl_->avio;impl_->format->flags|=AVFMT_FLAG_CUSTOM_IO|AVFMT_FLAG_NOBUFFER;impl_->format->probesize=32*1024;impl_->format->max_analyze_duration=250000;impl_->read_timeout_ms=5000;const AVInputFormat *flv=av_find_input_format("flv");AVFormatContext *opened=impl_->format;if(!flv||avformat_open_input(&opened,nullptr,flv,nullptr)<0){impl_->format=opened;stop();return false;}impl_->format=opened;if(avformat_find_stream_info(impl_->format,nullptr)<0){stop();return false;}impl_->configs.clear();for(unsigned int i=0;i<impl_->format->nb_streams;++i){AVStream *s=impl_->format->streams[i];AVCodecParameters *par=s?s->codecpar:nullptr;if(!par)continue;if(par->codec_type==AVMEDIA_TYPE_VIDEO&&par->codec_id==AV_CODEC_ID_H264&&impl_->video_stream<0){impl_->video_stream=static_cast<int>(i);MediaConfig c;c.kind=MediaKind::VideoH264;c.extradata=copy_extra(par);impl_->configs.push_back(std::move(c));}else if(par->codec_type==AVMEDIA_TYPE_AUDIO&&par->codec_id==AV_CODEC_ID_AAC&&impl_->audio_stream<0){impl_->audio_stream=static_cast<int>(i);MediaConfig c;c.kind=MediaKind::AudioAac;c.extradata=copy_extra(par);c.sample_rate=par->sample_rate;c.channels=par->ch_layout.nb_channels;impl_->configs.push_back(std::move(c));}}if(impl_->video_stream<0){stop();return false;}if(audio&&impl_->audio_stream<0){stop();return false;}return true;}

bool VideoCapture::next_view(EncodedMediaView &unit,int timeout_ms){
    if(!impl_||!impl_->format||!impl_->packet)return false;
    impl_->read_timeout_ms=std::max(1,timeout_ms);
    unit.kind=MediaKind::VideoH264;unit.data={};unit.pts_us=0;unit.capture_time_us=0;unit.keyframe=false;
    for(;;){
        av_packet_unref(impl_->packet);int rc=av_read_frame(impl_->format,impl_->packet);
        if(rc<0){if(rc!=AVERROR(EAGAIN))impl_->terminal=true;return false;}
        const int stream_index=impl_->packet->stream_index;if(stream_index!=impl_->video_stream&&stream_index!=impl_->audio_stream)continue;
        AVStream *s=impl_->format->streams[stream_index];unit.kind=stream_index==impl_->video_stream?MediaKind::VideoH264:MediaKind::AudioAac;
        if(impl_->packet->data&&impl_->packet->size>0)unit.data=std::span<const std::uint8_t>(impl_->packet->data,static_cast<std::size_t>(impl_->packet->size));
        std::int64_t timestamp=impl_->packet->pts!=AV_NOPTS_VALUE?impl_->packet->pts:impl_->packet->dts;
        if(timestamp!=AV_NOPTS_VALUE&&s)unit.pts_us=av_rescale_q(timestamp,s->time_base,AVRational{1,1000000});
        const auto now=monotonic_us();
        if(timestamp!=AV_NOPTS_VALUE&&s){if(!impl_->timeline_anchored){impl_->timeline_anchored=true;impl_->timeline_origin_pts_us=unit.pts_us;impl_->timeline_origin_monotonic_us=now;}const auto delta=unit.pts_us-impl_->timeline_origin_pts_us;unit.capture_time_us=delta>0?impl_->timeline_origin_monotonic_us+static_cast<std::uint64_t>(delta):impl_->timeline_origin_monotonic_us;}else unit.capture_time_us=now;
        unit.keyframe=unit.kind==MediaKind::VideoH264&&(impl_->packet->flags&AV_PKT_FLAG_KEY)!=0;return !unit.data.empty();
    }
}
bool VideoCapture::next(EncodedMediaUnit &unit,int timeout_ms){EncodedMediaView view;if(!next_view(view,timeout_ms))return false;unit.kind=view.kind;unit.data.clear();unit.data.assign(view.data.begin(),view.data.end());unit.pts_us=view.pts_us;unit.capture_time_us=view.capture_time_us;unit.keyframe=view.keyframe;return !unit.data.empty();}
bool VideoCapture::ended()const{return impl_&&impl_->terminal;}const std::vector<MediaConfig>&VideoCapture::configs()const{static const std::vector<MediaConfig>empty;return impl_?impl_->configs:empty;}
std::string VideoCapture::backend_name()const{return impl_?impl_->backend:"unavailable";}
bool VideoCapture::capture_timestamp_estimated()const{return !impl_||impl_->timestamp_estimated;}
void VideoCapture::stop(){if(!impl_)return;if(impl_->packet)av_packet_unref(impl_->packet);if(impl_->format){avformat_close_input(&impl_->format);impl_->format=nullptr;}if(impl_->avio){av_freep(&impl_->avio->buffer);avio_context_free(&impl_->avio);impl_->avio=nullptr;}stop_capture(impl_->capture);impl_->video_stream=-1;impl_->audio_stream=-1;impl_->terminal=false;impl_->timeline_anchored=false;impl_->timeline_origin_pts_us=0;impl_->timeline_origin_monotonic_us=0;impl_->configs.clear();impl_->backend="unconfigured";impl_->timestamp_estimated=true;}
}
