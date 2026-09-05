#include <opal/video_capture.hpp>
#include <opal/config.hpp>
#include <opal/flv_stream.hpp>
#include <opal/media.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <deque>
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
    std::deque<EncodedMediaUnit> prefetched;
    EncodedMediaUnit held_prefetch;
    std::size_t prefetched_bytes=0;
    bool terminal=false,timeline_anchored=false;
    std::int64_t timeline_origin_dts_us=0;
    std::uint64_t timeline_origin_monotonic_us=0;
    std::string backend="unconfigured",error;
    bool timestamp_estimated=true;

    bool configs_ready(bool audio) const {
        bool video=false,audio_ready=!audio;
        for(const auto&config:configs){
            if(config.kind==MediaKind::VideoH264&&!config.extradata.empty())video=true;
            if(config.kind==MediaKind::AudioAac&&!config.extradata.empty()&&config.sample_rate>0&&config.channels>0)audio_ready=true;
        }
        return video&&audio_ready;
    }

    void apply_config(const FlvEvent&event){
        const MediaKind kind=event.type==FlvEventType::VideoConfig?MediaKind::VideoH264:MediaKind::AudioAac;
        MediaConfig*target=nullptr;
        for(auto&config:configs)if(config.kind==kind){target=&config;break;}
        if(!target){configs.push_back({});target=&configs.back();target->kind=kind;}
        target->extradata.assign(event.data.begin(),event.data.end());
        if(kind==MediaKind::AudioAac){target->sample_rate=event.sample_rate;target->channels=event.channels;}
    }

    std::uint64_t capture_time(std::int64_t dts_us){
        const auto now=monotonic_us();
        if(!timeline_anchored){timeline_anchored=true;timeline_origin_dts_us=dts_us;timeline_origin_monotonic_us=now;}
        const auto delta=dts_us-timeline_origin_dts_us;
        return delta>0?timeline_origin_monotonic_us+static_cast<std::uint64_t>(delta):timeline_origin_monotonic_us;
    }

    bool queue_media(const FlvEvent&event){
        constexpr std::size_t kMaxStartupPackets=16,kMaxStartupBytes=4u*1024u*1024u;
        if(prefetched.size()>=kMaxStartupPackets||prefetched_bytes+event.data.size()>kMaxStartupBytes)return false;
        EncodedMediaUnit unit;
        unit.kind=event.type==FlvEventType::Video?MediaKind::VideoH264:MediaKind::AudioAac;
        unit.data.assign(event.data.begin(),event.data.end());unit.pts_us=event.pts_us;unit.capture_time_us=capture_time(event.dts_us);unit.keyframe=event.keyframe;
        prefetched_bytes+=unit.data.size();prefetched.push_back(std::move(unit));return true;
    }

    bool event_to_view(const FlvEvent&event,EncodedMediaView&unit){
        if(event.type!=FlvEventType::Video&&event.type!=FlvEventType::Audio)return false;
        unit.kind=event.type==FlvEventType::Video?MediaKind::VideoH264:MediaKind::AudioAac;unit.data=event.data;unit.pts_us=event.pts_us;unit.capture_time_us=capture_time(event.dts_us);unit.keyframe=event.keyframe;return !unit.data.empty();
    }
};

VideoCapture::VideoCapture():impl_(std::make_unique<Impl>()){}
VideoCapture::~VideoCapture(){stop();}

bool VideoCapture::start(const StreamOptions&stream,int bitrate_kbps,bool audio,const std::string&portal_token_file){
    stop();if(!impl_)impl_=std::make_unique<Impl>();
    impl_->error.clear();impl_->terminal=false;impl_->timeline_anchored=false;impl_->timeline_origin_dts_us=0;impl_->timeline_origin_monotonic_us=0;impl_->timestamp_estimated=true;impl_->parser.reset();impl_->configs.clear();impl_->prefetched.clear();impl_->held_prefetch={};impl_->prefetched_bytes=0;
    auto fail=[&](std::string message){impl_->error=std::move(message);if(debug_enabled())std::cerr<<"OPAL capture startup failed backend="<<impl_->backend<<" reason="<<impl_->error<<"\n";stop_capture(impl_->capture);return false;};

    std::string command;
    if(const char*override_command=std::getenv("OPAL_CAPTURE_CMD");override_command&&*override_command){command=override_command;impl_->backend="external-override-flv";}
    else{const bool gsr=command_exists("gpu-screen-recorder");command=capture_command(gsr,stream.fps,bitrate_kbps,audio,portal_token_file,stream.max_width,stream.max_height);impl_->backend=gsr?"gpu-screen-recorder-flv":"ffmpeg-x11grab-flv";}
    if(command.empty())return fail("no usable capture command");

    impl_->capture=start_capture(command);if(impl_->capture.pid<=0||impl_->capture.fd<0)return fail("capture process did not start");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(2500);
    while(!impl_->configs_ready(audio)){
        for(;;){
            const auto event=impl_->parser.next();
            if(event.type==FlvEventType::NeedMore)break;
            if(event.type==FlvEventType::Invalid)return fail(impl_->parser.error().empty()?"invalid FLV stream":impl_->parser.error());
            if(event.type==FlvEventType::VideoConfig||event.type==FlvEventType::AudioConfig){impl_->apply_config(event);if(impl_->configs_ready(audio))break;continue;}
            if((event.type==FlvEventType::Video||event.type==FlvEventType::Audio)&&!impl_->queue_media(event))return fail("too much media arrived before FLV configuration");
        }
        if(impl_->configs_ready(audio))break;
        if(std::chrono::steady_clock::now()>=deadline)return fail(audio?"FLV AVC/AAC configuration startup deadline exceeded":"FLV AVC configuration startup deadline exceeded");
        const int n=read_capture(impl_->capture,impl_->read_buffer.data(),impl_->read_buffer.size(),250);
        if(n>0){if(!impl_->parser.append(std::span<const std::uint8_t>(impl_->read_buffer.data(),static_cast<std::size_t>(n))))return fail(impl_->parser.error().empty()?"FLV parser buffer rejected input":impl_->parser.error());continue;}
        if(n==-2)continue;
        if(n==0)return fail("capture ended before FLV configuration");
        return fail("capture pipe read failed during FLV startup");
    }

    if(debug_enabled())std::cerr<<"OPAL capture backend="<<impl_->backend<<" acquisition_timestamp=estimated demux=native-flv startup_prefetch="<<impl_->prefetched.size()<<"\n";
    return true;
}

bool VideoCapture::next_view(EncodedMediaView&unit,int timeout_ms){
    if(!impl_||impl_->capture.fd<0)return false;
    unit={};
    if(!impl_->prefetched.empty()){
        impl_->held_prefetch=std::move(impl_->prefetched.front());impl_->prefetched.pop_front();impl_->prefetched_bytes-=impl_->held_prefetch.data.size();
        unit.kind=impl_->held_prefetch.kind;unit.data=impl_->held_prefetch.data;unit.pts_us=impl_->held_prefetch.pts_us;unit.capture_time_us=impl_->held_prefetch.capture_time_us;unit.keyframe=impl_->held_prefetch.keyframe;return !unit.data.empty();
    }
    impl_->held_prefetch={};
    for(;;){
        const auto event=impl_->parser.next();
        if(event.type==FlvEventType::Invalid){impl_->error=impl_->parser.error();impl_->terminal=true;return false;}
        if(event.type==FlvEventType::VideoConfig||event.type==FlvEventType::AudioConfig){impl_->apply_config(event);continue;}
        if(event.type==FlvEventType::Video||event.type==FlvEventType::Audio)return impl_->event_to_view(event,unit);
        const int n=read_capture(impl_->capture,impl_->read_buffer.data(),impl_->read_buffer.size(),std::max(1,timeout_ms));
        if(n>0){if(!impl_->parser.append(std::span<const std::uint8_t>(impl_->read_buffer.data(),static_cast<std::size_t>(n)))){impl_->error=impl_->parser.error();impl_->terminal=true;return false;}continue;}
        if(n==-2)return false;
        if(n==0){impl_->terminal=true;return false;}
        impl_->error="capture pipe read failed";impl_->terminal=true;return false;
    }
}

bool VideoCapture::next(EncodedMediaUnit&unit,int timeout_ms){EncodedMediaView view;if(!next_view(view,timeout_ms))return false;unit.kind=view.kind;unit.data.clear();unit.data.assign(view.data.begin(),view.data.end());unit.pts_us=view.pts_us;unit.capture_time_us=view.capture_time_us;unit.keyframe=view.keyframe;return !unit.data.empty();}
bool VideoCapture::ended()const{return impl_&&impl_->terminal;}
const std::vector<MediaConfig>&VideoCapture::configs()const{static const std::vector<MediaConfig>empty;return impl_?impl_->configs:empty;}
std::string VideoCapture::backend_name()const{return impl_?impl_->backend:"unavailable";}
std::string VideoCapture::last_error()const{return impl_?impl_->error:"capture unavailable";}
bool VideoCapture::capture_timestamp_estimated()const{return !impl_||impl_->timestamp_estimated;}
void VideoCapture::stop(){if(!impl_)return;stop_capture(impl_->capture);impl_->parser.reset();impl_->configs.clear();impl_->prefetched.clear();impl_->held_prefetch={};impl_->prefetched_bytes=0;impl_->terminal=false;impl_->timeline_anchored=false;impl_->timeline_origin_dts_us=0;impl_->timeline_origin_monotonic_us=0;impl_->backend="unconfigured";impl_->timestamp_estimated=true;}
}
