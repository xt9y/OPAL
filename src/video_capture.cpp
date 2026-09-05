#include <opal/video_capture.hpp>
#include <opal/config.hpp>
#include <opal/flv_stream.hpp>
#include <opal/media.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

namespace opal {
namespace {
std::uint64_t monotonic_us(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
bool debug_enabled(){const char*value=std::getenv("OPAL_DEBUG");return value&&*value&&std::string(value)!="0";}
}

struct VideoCapture::Impl {
    CaptureProcess capture;
    FlvStreamParser parser;
    std::array<std::uint8_t,64u*1024u> read_buffer{};
    std::vector<MediaConfig> configs;
    std::uint64_t config_revision=0;
    bool terminal=false,timeline_anchored=false;
    std::int64_t timeline_origin_dts_us=0;
    std::uint64_t timeline_origin_monotonic_us=0;
    std::string backend="unconfigured",error;
    bool timestamp_estimated=true;

    void apply_config(const FlvEvent&event){
        const MediaKind kind=event.type==FlvEventType::VideoConfig?MediaKind::VideoH264:MediaKind::AudioAac;
        MediaConfig incoming;
        incoming.kind=kind;
        incoming.extradata.assign(event.data.begin(),event.data.end());
        if(kind==MediaKind::AudioAac){incoming.sample_rate=event.sample_rate;incoming.channels=event.channels;}
        for(auto&config:configs){
            if(config.kind!=kind)continue;
            if(config.extradata==incoming.extradata&&config.sample_rate==incoming.sample_rate&&config.channels==incoming.channels)return;
            config=std::move(incoming);
            ++config_revision;
            return;
        }
        configs.push_back(std::move(incoming));
        ++config_revision;
    }

    std::uint64_t capture_time(std::int64_t dts_us){
        const auto now=monotonic_us();
        if(!timeline_anchored){timeline_anchored=true;timeline_origin_dts_us=dts_us;timeline_origin_monotonic_us=now;}
        const auto delta=dts_us-timeline_origin_dts_us;
        return delta>0?timeline_origin_monotonic_us+static_cast<std::uint64_t>(delta):timeline_origin_monotonic_us;
    }

    bool event_to_view(const FlvEvent&event,EncodedMediaView&unit){
        if(event.type!=FlvEventType::Video&&event.type!=FlvEventType::Audio)return false;
        unit.kind=event.type==FlvEventType::Video?MediaKind::VideoH264:MediaKind::AudioAac;
        unit.data=event.data;
        unit.pts_us=event.pts_us;
        unit.capture_time_us=capture_time(event.dts_us);
        unit.keyframe=event.keyframe;
        return !unit.data.empty();
    }
};

VideoCapture::VideoCapture():impl_(std::make_unique<Impl>()){}
VideoCapture::~VideoCapture(){stop();}

bool VideoCapture::start(const StreamOptions&stream,int bitrate_kbps,bool audio,const std::string&portal_token_file){
    stop();
    if(!impl_)impl_=std::make_unique<Impl>();
    impl_->error.clear();
    impl_->terminal=false;
    impl_->timeline_anchored=false;
    impl_->timeline_origin_dts_us=0;
    impl_->timeline_origin_monotonic_us=0;
    impl_->timestamp_estimated=true;
    impl_->parser.reset();
    impl_->configs.clear();
    impl_->config_revision=0;

    auto fail=[&](std::string message){
        impl_->error=std::move(message);
        if(debug_enabled())std::cerr<<"OPAL capture startup failed backend="<<impl_->backend<<" reason="<<impl_->error<<"\n";
        stop_capture(impl_->capture);
        return false;
    };

    std::string command;
    if(const char*override_command=std::getenv("OPAL_CAPTURE_CMD");override_command&&*override_command){
        command=override_command;
        impl_->backend="external-override-flv";
    }else{
        const bool gsr=command_exists("gpu-screen-recorder");
        command=capture_command(gsr,stream.fps,bitrate_kbps,audio,portal_token_file,stream.max_width,stream.max_height);
        impl_->backend=gsr?"gpu-screen-recorder-flv":"ffmpeg-x11grab-flv";
    }
    if(command.empty())return fail("no usable capture command");

    impl_->capture=start_capture(command);
    if(impl_->capture.pid<=0||impl_->capture.fd<0)return fail("capture process did not start");
    if(debug_enabled())std::cerr<<"OPAL capture backend="<<impl_->backend<<" acquisition_timestamp=estimated demux=native-flv startup=async\n";
    return true;
}

bool VideoCapture::next_view(EncodedMediaView&unit,int timeout_ms){
    if(!impl_||impl_->capture.fd<0)return false;
    unit={};
    for(;;){
        const auto event=impl_->parser.next();
        if(event.type==FlvEventType::Invalid){
            impl_->error=impl_->parser.error();
            impl_->terminal=true;
            return false;
        }
        if(event.type==FlvEventType::VideoConfig||event.type==FlvEventType::AudioConfig){
            impl_->apply_config(event);
            continue;
        }
        if(event.type==FlvEventType::Video||event.type==FlvEventType::Audio)return impl_->event_to_view(event,unit);

        const int n=read_capture(impl_->capture,impl_->read_buffer.data(),impl_->read_buffer.size(),std::max(1,timeout_ms));
        if(n>0){
            if(!impl_->parser.append(std::span<const std::uint8_t>(impl_->read_buffer.data(),static_cast<std::size_t>(n)))){
                impl_->error=impl_->parser.error();
                impl_->terminal=true;
                return false;
            }
            continue;
        }
        if(n==-2)return false;
        if(n==0){impl_->terminal=true;return false;}
        impl_->error="capture pipe read failed";
        impl_->terminal=true;
        return false;
    }
}

bool VideoCapture::next(EncodedMediaUnit&unit,int timeout_ms){
    EncodedMediaView view;
    if(!next_view(view,timeout_ms))return false;
    unit.kind=view.kind;
    unit.data.clear();
    unit.data.assign(view.data.begin(),view.data.end());
    unit.pts_us=view.pts_us;
    unit.capture_time_us=view.capture_time_us;
    unit.keyframe=view.keyframe;
    return !unit.data.empty();
}

bool VideoCapture::ended()const{return impl_&&impl_->terminal;}
const std::vector<MediaConfig>&VideoCapture::configs()const{static const std::vector<MediaConfig>empty;return impl_?impl_->configs:empty;}
std::uint64_t VideoCapture::config_revision()const{return impl_?impl_->config_revision:0;}
std::string VideoCapture::backend_name()const{return impl_?impl_->backend:"unavailable";}
std::string VideoCapture::last_error()const{return impl_?impl_->error:"capture unavailable";}
bool VideoCapture::capture_timestamp_estimated()const{return !impl_||impl_->timestamp_estimated;}

void VideoCapture::stop(){
    if(!impl_)return;
    stop_capture(impl_->capture);
    impl_->parser.reset();
    impl_->configs.clear();
    impl_->config_revision=0;
    impl_->terminal=false;
    impl_->timeline_anchored=false;
    impl_->timeline_origin_dts_us=0;
    impl_->timeline_origin_monotonic_us=0;
    impl_->backend="unconfigured";
    impl_->timestamp_estimated=true;
}
}
