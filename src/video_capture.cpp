#include <opal/video_capture.hpp>
#include <opal/config.hpp>
#include <opal/flv_stream.hpp>
#include <opal/media.hpp>
#include <opal/pipewire_capture.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

namespace opal {
namespace {
std::uint64_t monotonic_us(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
bool debug_enabled(){const char*value=std::getenv("OPAL_DEBUG");return value&&*value&&std::string(value)!="0";}
bool wayland_session(){const char*display=std::getenv("WAYLAND_DISPLAY");return display&&*display;}
std::string audio_only_command(){return command_exists("ffmpeg")?"ffmpeg -nostdin -hide_banner -loglevel error -thread_queue_size 64 -f pulse -i default -vn -c:a aac -b:a 128k -ar 48000 -ac 2 -flush_packets 1 -f flv pipe:1":"";}
}

struct VideoCapture::Impl {
    CaptureProcess capture;
    FlvStreamParser parser;
    NativePipeWireVideoCapture native_video;
    EncodedMediaUnit native_unit;
    std::array<std::uint8_t,64u*1024u> read_buffer{};
    std::vector<MediaConfig> configs;
    std::uint64_t config_revision=0,native_config_revision=0;
    bool terminal=false,timeline_anchored=false,native_active=false,audio_requested=false,audio_only=false;
    std::int64_t timeline_origin_dts_us=0;
    std::uint64_t timeline_origin_monotonic_us=0;
    std::string backend="unconfigured",error,portal_token;
    StreamOptions stream{};
    int bitrate_kbps=0;
    bool timestamp_estimated=true;

    void reset_timeline(){timeline_anchored=false;timeline_origin_dts_us=0;timeline_origin_monotonic_us=0;}
    void merge_config(MediaConfig incoming){
        if(incoming.extradata.empty())return;
        for(auto&config:configs){
            if(config.kind!=incoming.kind)continue;
            if(config.extradata==incoming.extradata&&config.sample_rate==incoming.sample_rate&&config.channels==incoming.channels)return;
            config=std::move(incoming);++config_revision;return;
        }
        configs.push_back(std::move(incoming));++config_revision;
    }
    void apply_config(const FlvEvent&event){
        const MediaKind kind=event.type==FlvEventType::VideoConfig?MediaKind::VideoH264:MediaKind::AudioAac;
        MediaConfig incoming;incoming.kind=kind;incoming.extradata.assign(event.data.begin(),event.data.end());if(kind==MediaKind::AudioAac){incoming.sample_rate=event.sample_rate;incoming.channels=event.channels;}merge_config(std::move(incoming));
    }
    void sync_native_config(){const auto revision=native_video.config_revision();if(!revision||revision==native_config_revision)return;native_config_revision=revision;merge_config(native_video.config());}

    std::uint64_t capture_time(std::int64_t dts_us){
        const auto now=monotonic_us();
        if(!timeline_anchored){timeline_anchored=true;timeline_origin_dts_us=dts_us;timeline_origin_monotonic_us=now;}
        const auto delta=dts_us-timeline_origin_dts_us;
        return delta>0?timeline_origin_monotonic_us+static_cast<std::uint64_t>(delta):timeline_origin_monotonic_us;
    }

    bool event_to_view(const FlvEvent&event,EncodedMediaView&unit){
        if(event.type!=FlvEventType::Video&&event.type!=FlvEventType::Audio)return false;
        unit.kind=event.type==FlvEventType::Video?MediaKind::VideoH264:MediaKind::AudioAac;
        unit.data=event.data;unit.pts_us=event.pts_us;unit.capture_time_us=capture_time(event.dts_us);unit.keyframe=event.keyframe;return !unit.data.empty();
    }
    bool native_to_view(EncodedMediaView&unit,int timeout_ms){
        native_unit={};if(!native_video.next(native_unit,timeout_ms))return false;sync_native_config();unit.kind=native_unit.kind;unit.data=native_unit.data;unit.pts_us=native_unit.pts_us;unit.capture_time_us=native_unit.capture_time_us;unit.keyframe=native_unit.keyframe;return !unit.data.empty();
    }

    bool start_process(const std::string&command,bool audio_process){
        stop_capture(capture);parser.reset();reset_timeline();if(command.empty())return false;capture=start_capture(command);if(capture.pid<=0||capture.fd<0)return false;audio_only=audio_process;return true;
    }
    std::string full_capture_command(){const bool gsr=command_exists("gpu-screen-recorder");backend=gsr?"gpu-screen-recorder-flv":"ffmpeg-x11grab-flv";return capture_command(gsr,stream.fps,bitrate_kbps,audio_requested,portal_token,stream.max_width,stream.max_height);}
    bool start_external_fallback(){
        native_video.stop();native_active=false;timestamp_estimated=true;native_config_revision=0;configs.clear();++config_revision;const auto command=full_capture_command();if(!start_process(command,false)){error="capture fallback did not start";terminal=true;return false;}if(debug_enabled())std::cerr<<"OPAL capture fallback backend="<<backend<<" acquisition_timestamp=estimated demux=native-flv\n";return true;
    }
    bool start_native(){
        if(!NativePipeWireVideoCapture::compiled()||!wayland_session())return false;if(!native_video.start(stream,bitrate_kbps,portal_token))return false;native_active=true;timestamp_estimated=false;backend="pipewire-native";native_config_revision=0;
        if(audio_requested){const auto command=audio_only_command();if(command.empty()||!start_process(command,true)){native_video.stop();native_active=false;return false;}}
        if(debug_enabled())std::cerr<<"OPAL capture backend=pipewire-native acquisition_timestamp=pipewire-cycle-exact video_encode=in-process audio="<<(audio_requested?"ffmpeg-audio-only":"off")<<" startup=async\n";return true;
    }

    bool pump_external(EncodedMediaView&unit,int timeout_ms,bool allow_video){
        if(capture.fd<0)return false;
        for(;;){
            const auto event=parser.next();
            if(event.type==FlvEventType::Invalid){error=parser.error();terminal=true;return false;}
            if(event.type==FlvEventType::VideoConfig||event.type==FlvEventType::AudioConfig){if(allow_video||event.type==FlvEventType::AudioConfig)apply_config(event);continue;}
            if(event.type==FlvEventType::Video||event.type==FlvEventType::Audio){if(!allow_video&&event.type==FlvEventType::Video)continue;return event_to_view(event,unit);}
            const int n=read_capture(capture,read_buffer.data(),read_buffer.size(),std::max(0,timeout_ms));
            if(n>0){if(!parser.append(std::span<const std::uint8_t>(read_buffer.data(),static_cast<std::size_t>(n)))){error=parser.error();terminal=true;return false;}continue;}
            if(n==-2)return false;
            if(n==0){if(native_active&&audio_only){stop_capture(capture);capture={};audio_only=false;return false;}terminal=true;return false;}
            error="capture pipe read failed";if(native_active&&audio_only){stop_capture(capture);capture={};audio_only=false;return false;}terminal=true;return false;
        }
    }
};

VideoCapture::VideoCapture():impl_(std::make_unique<Impl>()){}
VideoCapture::~VideoCapture(){stop();}

bool VideoCapture::start(const StreamOptions&stream,int bitrate_kbps,bool audio,const std::string&portal_token_file){
    stop();if(!impl_)impl_=std::make_unique<Impl>();impl_->error.clear();impl_->terminal=false;impl_->timestamp_estimated=true;impl_->parser.reset();impl_->configs.clear();impl_->config_revision=0;impl_->native_config_revision=0;impl_->stream=stream;impl_->bitrate_kbps=bitrate_kbps;impl_->audio_requested=audio;impl_->portal_token=portal_token_file;impl_->reset_timeline();
    auto fail=[&](std::string message){impl_->error=std::move(message);if(debug_enabled())std::cerr<<"OPAL capture startup failed backend="<<impl_->backend<<" reason="<<impl_->error<<"\n";stop_capture(impl_->capture);impl_->native_video.stop();impl_->native_active=false;return false;};
    if(const char*override_command=std::getenv("OPAL_CAPTURE_CMD");override_command&&*override_command){impl_->backend="external-override-flv";if(!impl_->start_process(override_command,false))return fail("capture process did not start");if(debug_enabled())std::cerr<<"OPAL capture backend="<<impl_->backend<<" acquisition_timestamp="<<capture_timestamp_quality_name(capture_timestamp_quality())<<" demux=native-flv startup=async\n";return true;}
    if(impl_->start_native())return true;
    const auto command=impl_->full_capture_command();if(command.empty())return fail("no usable capture command");if(!impl_->start_process(command,false))return fail("capture process did not start");if(debug_enabled())std::cerr<<"OPAL capture backend="<<impl_->backend<<" acquisition_timestamp="<<capture_timestamp_quality_name(capture_timestamp_quality())<<" demux=native-flv startup=async\n";return true;
}

bool VideoCapture::next_view(EncodedMediaView&unit,int timeout_ms){
    if(!impl_)return false;unit={};
    if(!impl_->native_active)return impl_->pump_external(unit,std::max(1,timeout_ms),true);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(std::max(0,timeout_ms));
    for(;;){
        impl_->sync_native_config();if(impl_->native_to_view(unit,0))return true;if(impl_->capture.fd>=0&&impl_->pump_external(unit,0,false))return true;
        if(impl_->native_video.ended()){
            const auto reason=impl_->native_video.last_error();if(debug_enabled())std::cerr<<"OPAL native capture ended reason="<<(reason.empty()?"unknown":reason)<<"; falling back\n";if(!impl_->start_external_fallback())return false;return impl_->pump_external(unit,std::max(1,timeout_ms),true);
        }
        const auto now=std::chrono::steady_clock::now();if(now>=deadline)return false;const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count();const int wait_ms=std::max(1,std::min<int>(5,static_cast<int>(remaining)));if(impl_->native_to_view(unit,wait_ms))return true;if(impl_->capture.fd>=0&&impl_->pump_external(unit,0,false))return true;
    }
}

bool VideoCapture::next(EncodedMediaUnit&unit,int timeout_ms){EncodedMediaView view;if(!next_view(view,timeout_ms))return false;unit.kind=view.kind;unit.data.clear();unit.data.assign(view.data.begin(),view.data.end());unit.pts_us=view.pts_us;unit.capture_time_us=view.capture_time_us;unit.keyframe=view.keyframe;return !unit.data.empty();}

bool VideoCapture::ended()const{return impl_&&impl_->terminal;}
const std::vector<MediaConfig>&VideoCapture::configs()const{static const std::vector<MediaConfig>empty;if(impl_&&impl_->native_active)impl_->sync_native_config();return impl_?impl_->configs:empty;}
std::uint64_t VideoCapture::config_revision()const{if(impl_&&impl_->native_active)impl_->sync_native_config();return impl_?impl_->config_revision:0;}
std::string VideoCapture::backend_name()const{if(!impl_)return"unavailable";if(impl_->native_active){const auto native=impl_->native_video.backend_name();return native+(impl_->audio_requested?"+audio-flv":"");}return impl_->backend;}
std::string VideoCapture::last_error()const{return impl_?impl_->error:"capture unavailable";}
CaptureTimestampQuality VideoCapture::capture_timestamp_quality()const{return impl_&&!impl_->timestamp_estimated?CaptureTimestampQuality::Exact:CaptureTimestampQuality::Estimated;}
bool VideoCapture::capture_timestamp_estimated()const{return capture_timestamp_quality()==CaptureTimestampQuality::Estimated;}

void VideoCapture::stop(){if(!impl_)return;impl_->native_video.stop();stop_capture(impl_->capture);impl_->parser.reset();impl_->configs.clear();impl_->config_revision=0;impl_->native_config_revision=0;impl_->terminal=false;impl_->native_active=false;impl_->audio_only=false;impl_->reset_timeline();impl_->backend="unconfigured";impl_->timestamp_estimated=true;impl_->native_unit={};}
}
