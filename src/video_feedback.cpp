#include <opal/video_feedback.hpp>

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>

namespace opal {
namespace {
bool valid_restart_reason(const std::string &reason){
    return reason=="none"||reason=="unknown"||reason=="reassembly-loss"||reason=="decode-failure"||
           reason=="decode-backlog"||reason=="bitrate-down"||reason=="bitrate-up"||
           reason=="capture-ended"||reason=="capture-stale"||reason=="send-failure";
}
std::uint32_t loss_per_mille(const VideoFeedbackSample&s){const std::uint64_t total=static_cast<std::uint64_t>(s.received)+s.lost;return total?static_cast<std::uint32_t>(std::min<std::uint64_t>(1000,static_cast<std::uint64_t>(s.lost)*1000/total)):0;}
}

BitrateController::BitrateController(int ceiling_kbps){
    ceiling_=std::max(1000,ceiling_kbps);
    floor_=std::min(ceiling_,std::max(4000,ceiling_*35/100));
    target_=std::clamp(ceiling_*60/100,floor_,ceiling_);
}

int BitrateController::on_feedback(const VideoFeedbackSample &sample,std::chrono::steady_clock::time_point now){
    if(sample.rtt_us){
        if(!baseline_rtt_us_||sample.rtt_us<baseline_rtt_us_)baseline_rtt_us_=sample.rtt_us;
        else baseline_rtt_us_=static_cast<std::uint32_t>((static_cast<std::uint64_t>(baseline_rtt_us_)*999+sample.rtt_us)/1000);
        smoothed_rtt_us_=smoothed_rtt_us_?static_cast<std::uint32_t>((static_cast<std::uint64_t>(smoothed_rtt_us_)*7+sample.rtt_us)/8):sample.rtt_us;
    }
    const auto loss=loss_per_mille(sample);
    const std::uint32_t queue_delay=(sample.rtt_us&&baseline_rtt_us_&&sample.rtt_us>baseline_rtt_us_)?sample.rtt_us-baseline_rtt_us_:0;
    const bool severe=loss>=20||queue_delay>=15000||sample.decode_age_us>=80000;
    const bool pressured=loss>=5||queue_delay>=8000||sample.decode_age_us>=45000;
    if(severe){target_=std::max(floor_,target_*80/100);last_adjust_=now;return target_;}
    if(pressured){target_=std::max(floor_,target_*90/100);last_adjust_=now;return target_;}
    const bool clean=loss<=1&&queue_delay<=3000&&(sample.decode_age_us==0||sample.decode_age_us<=25000);
    if(clean&&(last_adjust_.time_since_epoch().count()==0||now-last_adjust_>=std::chrono::milliseconds(300))){
        const int startup_ceiling=ceiling_*85/100;
        const int step=target_<startup_ceiling?std::max(500,target_/10):std::max(250,target_/20);
        target_=std::min(ceiling_,target_+step);last_adjust_=now;
    }
    return target_;
}

int BitrateController::target_kbps() const{return target_;}
int BitrateController::floor_kbps() const{return floor_;}

bool AdaptiveFecController::on_feedback(const VideoFeedbackSample&sample){
    const auto loss=loss_per_mille(sample);
    if(loss>=10){enabled_=true;clean_samples_=0;return true;}
    if(!enabled_)return false;
    if(loss<=1){if(++clean_samples_>=20){enabled_=false;clean_samples_=0;}}
    else clean_samples_=0;
    return enabled_;
}
bool AdaptiveFecController::enabled()const{return enabled_;}

ClockEstimate estimate_clock_offset(std::int64_t t0,std::int64_t t1,std::int64_t t2,std::int64_t t3){
    ClockEstimate out;
    if(t2<t1||t3<t0)return out;
    const std::int64_t local_elapsed=t3-t0,remote_elapsed=t2-t1;
    if(local_elapsed>60000000LL||remote_elapsed>60000000LL)return out;
    const std::int64_t rtt=local_elapsed-remote_elapsed;if(rtt<0||rtt>60000000LL)return out;
    out.offset_us=((t1-t0)+(t2-t3))/2;out.rtt_us=static_cast<std::uint32_t>(std::min<std::int64_t>(rtt,0xffffffffLL));out.valid=true;return out;
}

std::string video_feedback_line(std::uint32_t generation,const VideoFeedbackSample &sample){
    return "VIDEO_FEEDBACK "+std::to_string(generation)+" "+std::to_string(sample.highest_sequence)+" "+
        std::to_string(sample.received)+" "+std::to_string(sample.lost)+" "+std::to_string(sample.rtt_us)+" "+std::to_string(sample.decode_age_us);
}

bool parse_video_feedback_line(const std::string &line,std::uint32_t generation,VideoFeedbackSample &sample){
    std::istringstream in(line);std::string word,extra;unsigned long long gen=0,highest=0,received=0,lost=0,rtt=0,age=0;
    if(!(in>>word>>gen>>highest>>received>>lost>>rtt>>age)||in>>extra||word!="VIDEO_FEEDBACK"||gen!=generation||
       received>0xffffffffULL||lost>0xffffffffULL||rtt>0xffffffffULL||age>0xffffffffULL)return false;
    sample.highest_sequence=highest;sample.received=static_cast<std::uint32_t>(received);sample.lost=static_cast<std::uint32_t>(lost);
    sample.rtt_us=static_cast<std::uint32_t>(rtt);sample.decode_age_us=static_cast<std::uint32_t>(age);return true;
}

std::string clock_sync_request_line(std::uint32_t generation,std::int64_t t0_us){
    return "CLOCK_SYNC "+std::to_string(generation)+" "+std::to_string(t0_us)+" 0 0";
}
std::string clock_sync_reply_line(std::uint32_t generation,std::int64_t t0_us,std::int64_t t1_us,std::int64_t t2_us){
    return "CLOCK_SYNC "+std::to_string(generation)+" "+std::to_string(t0_us)+" "+std::to_string(t1_us)+" "+std::to_string(t2_us);
}
bool parse_clock_sync_line(const std::string &line,std::uint32_t generation,std::int64_t &t0,std::int64_t &t1,std::int64_t &t2){
    std::istringstream in(line);std::string word,extra;unsigned long long gen=0;
    if(!(in>>word>>gen>>t0>>t1>>t2)||in>>extra||word!="CLOCK_SYNC"||gen!=generation)return false;
    return true;
}

std::string debug_media_request_line(std::uint32_t generation,bool enabled){
    return "DEBUG_MEDIA "+std::to_string(generation)+" "+(enabled?"1":"0");
}
bool parse_debug_media_request_line(const std::string &line,std::uint32_t generation,bool &enabled){
    std::istringstream in(line);std::string word,extra;unsigned long long gen=0,value=0;
    if(!(in>>word>>gen>>value)||in>>extra||word!="DEBUG_MEDIA"||gen!=generation||value>1)return false;
    enabled=value==1;return true;
}

std::string host_media_debug_line(std::uint32_t generation,const HostMediaDebugSample &s){
    const std::string reason=valid_restart_reason(s.restart_reason)?s.restart_reason:"unknown";
    return "HOST_MEDIA "+std::to_string(generation)+" "+std::to_string(s.frame_id)+" "+std::to_string(s.frame_bytes)+" "+
        std::to_string(s.data_fragments)+" "+std::to_string(s.fec_fragments)+" "+std::to_string(s.send_span_us)+" "+
        std::to_string(s.capture_to_packet_us)+" "+std::to_string(s.target_kbps)+" "+std::to_string(s.active_kbps)+" "+
        std::to_string(s.stale_frames)+" "+std::to_string(s.idr_requests)+" "+std::to_string(s.restarts)+" "+(s.chain_valid?"1":"0")+" "+reason+" "+(s.capture_timestamp_exact?"exact":"estimated");
}

bool parse_host_media_debug_line(const std::string &line,std::uint32_t generation,HostMediaDebugSample &s){
    std::istringstream in(line);std::string word,reason,quality,extra;unsigned long long gen=0,frame=0,bytes=0,data=0,fec=0,send=0,capture=0,target=0,active=0,stale=0,idr=0,restarts=0,chain=0;
    if(!(in>>word>>gen>>frame>>bytes>>data>>fec>>send>>capture>>target>>active>>stale>>idr>>restarts>>chain)||
       word!="HOST_MEDIA"||gen!=generation||data>65535ULL||fec>65535ULL||send>60000000ULL||capture>60000000ULL||
       target>1000000ULL||active>1000000ULL||chain>1ULL)return false;
    reason="unknown";quality="estimated";
    if(in>>reason){
        if(!valid_restart_reason(reason))return false;
        if(in>>quality){if(quality!="exact"&&quality!="estimated")return false;if(in>>extra)return false;}
    }else in.clear();
    s.frame_id=frame;s.frame_bytes=bytes;s.data_fragments=static_cast<std::uint32_t>(data);s.fec_fragments=static_cast<std::uint32_t>(fec);
    s.send_span_us=static_cast<std::uint32_t>(send);s.capture_to_packet_us=static_cast<std::uint32_t>(capture);
    s.target_kbps=static_cast<int>(target);s.active_kbps=static_cast<int>(active);s.stale_frames=stale;s.idr_requests=idr;s.restarts=restarts;s.chain_valid=chain==1;s.restart_reason=reason;s.capture_timestamp_exact=quality=="exact";return true;
}

std::string format_host_media_debug(const HostMediaDebugSample &s){
    std::ostringstream out;out.setf(std::ios::fixed);out<<std::setprecision(1)
        <<"OPAL host frame="<<s.frame_id<<" bytes="<<s.frame_bytes
        <<" packets="<<s.data_fragments<<"+"<<s.fec_fragments
        <<" send="<<(static_cast<double>(s.send_span_us)/1000.0)<<"ms"
        <<" encoded->packet-est="<<(static_cast<double>(s.capture_to_packet_us)/1000.0)<<"ms"
        <<" capture_clock="<<(s.capture_timestamp_exact?"exact":"estimated")
        <<" bitrate="<<s.active_kbps<<"kbps target="<<s.target_kbps<<"kbps"
        <<" stale="<<s.stale_frames<<" idr="<<s.idr_requests<<" restarts="<<s.restarts
        <<" chain="<<(s.chain_valid?"ok":"waiting-idr")<<" restart_reason="<<s.restart_reason;return out.str();
}

std::string format_latency_telemetry(const LatencyTelemetry &t){
    std::ostringstream out;out.setf(std::ios::fixed);out<<std::setprecision(1)
        <<"OPAL latency packet_age_"<<(t.capture_timestamp_exact?"exact":"est")<<"="<<t.capture_to_packet_ms<<"ms network_age_"<<(t.capture_timestamp_exact?"exact":"est")<<"="<<t.network_ms
        <<"ms reassembly="<<t.reassembly_ms<<"ms decode="<<t.decode_ms<<"ms present_submit="<<t.present_ms
        <<"ms media_age_"<<(t.capture_timestamp_exact?"exact":"est")<<"="<<t.total_ms<<"ms capture_clock="<<(t.capture_timestamp_exact?"exact":"estimated")
        <<" loss="<<t.loss_percent<<"% stale="<<t.stale_frames
        <<" bitrate="<<t.bitrate_kbps<<"kbps decoder="<<t.decoder_backend
        <<" decode_fps="<<t.decoded_fps<<" present_fps="<<t.presented_fps
        <<" queue="<<t.video_queue_depth<<" skip_present="<<t.skipped_present_frames;return out.str();
}

}