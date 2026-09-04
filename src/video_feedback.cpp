#include <opal/video_feedback.hpp>

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>

namespace opal {

BitrateController::BitrateController(int ceiling_kbps){
    ceiling_=std::max(1000,ceiling_kbps);floor_=std::max(4000,ceiling_*35/100);floor_=std::min(floor_,ceiling_);target_=ceiling_;
}

int BitrateController::on_feedback(const VideoFeedbackSample &sample,std::chrono::steady_clock::time_point now){
    const std::uint64_t total=static_cast<std::uint64_t>(sample.received)+sample.lost;
    const bool high_loss=total>0&&static_cast<std::uint64_t>(sample.lost)*100>total*2;
    const bool low_loss=total==0||static_cast<std::uint64_t>(sample.lost)*1000<total*2;
    if(sample.rtt_us>0&&(baseline_rtt_us_==0||sample.rtt_us<baseline_rtt_us_))baseline_rtt_us_=sample.rtt_us;
    const bool high_rtt=sample.rtt_us>0&&baseline_rtt_us_>0&&sample.rtt_us>baseline_rtt_us_+15000;
    const bool low_rtt=sample.rtt_us==0||baseline_rtt_us_==0||sample.rtt_us<baseline_rtt_us_+3000;
    if(high_loss||high_rtt){
        good_since_={};
        target_=std::max(floor_,target_*75/100);
        return target_;
    }
    if(low_loss&&low_rtt){
        if(good_since_.time_since_epoch().count()==0)good_since_=now;
        if(now-good_since_>=std::chrono::seconds(1)){
            target_=std::min(ceiling_,std::max(target_+1,target_*105/100));good_since_=now;last_raise_=now;
        }
    }else good_since_={};
    return target_;
}

int BitrateController::target_kbps() const{return target_;}
int BitrateController::floor_kbps() const{return floor_;}

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
    return "HOST_MEDIA "+std::to_string(generation)+" "+std::to_string(s.frame_id)+" "+std::to_string(s.frame_bytes)+" "+
        std::to_string(s.data_fragments)+" "+std::to_string(s.fec_fragments)+" "+std::to_string(s.send_span_us)+" "+
        std::to_string(s.capture_to_packet_us)+" "+std::to_string(s.target_kbps)+" "+std::to_string(s.active_kbps)+" "+
        std::to_string(s.stale_frames)+" "+std::to_string(s.idr_requests)+" "+std::to_string(s.restarts)+" "+(s.chain_valid?"1":"0");
}

bool parse_host_media_debug_line(const std::string &line,std::uint32_t generation,HostMediaDebugSample &s){
    std::istringstream in(line);std::string word,extra;unsigned long long gen=0,frame=0,bytes=0,data=0,fec=0,send=0,capture=0,target=0,active=0,stale=0,idr=0,restarts=0,chain=0;
    if(!(in>>word>>gen>>frame>>bytes>>data>>fec>>send>>capture>>target>>active>>stale>>idr>>restarts>>chain)||in>>extra||
       word!="HOST_MEDIA"||gen!=generation||data>65535ULL||fec>65535ULL||send>60000000ULL||capture>60000000ULL||
       target>1000000ULL||active>1000000ULL||chain>1ULL)return false;
    s.frame_id=frame;s.frame_bytes=bytes;s.data_fragments=static_cast<std::uint32_t>(data);s.fec_fragments=static_cast<std::uint32_t>(fec);
    s.send_span_us=static_cast<std::uint32_t>(send);s.capture_to_packet_us=static_cast<std::uint32_t>(capture);
    s.target_kbps=static_cast<int>(target);s.active_kbps=static_cast<int>(active);s.stale_frames=stale;s.idr_requests=idr;s.restarts=restarts;s.chain_valid=chain==1;return true;
}

std::string format_host_media_debug(const HostMediaDebugSample &s){
    std::ostringstream out;out.setf(std::ios::fixed);out<<std::setprecision(1)
        <<"OPAL host frame="<<s.frame_id<<" bytes="<<s.frame_bytes
        <<" packets="<<s.data_fragments<<"+"<<s.fec_fragments
        <<" send="<<(static_cast<double>(s.send_span_us)/1000.0)<<"ms"
        <<" capture->packet="<<(static_cast<double>(s.capture_to_packet_us)/1000.0)<<"ms"
        <<" bitrate="<<s.active_kbps<<"kbps target="<<s.target_kbps<<"kbps"
        <<" stale="<<s.stale_frames<<" idr="<<s.idr_requests<<" restarts="<<s.restarts
        <<" chain="<<(s.chain_valid?"ok":"waiting-idr");return out.str();
}

std::string format_latency_telemetry(const LatencyTelemetry &t){
    std::ostringstream out;out.setf(std::ios::fixed);out<<std::setprecision(1)
        <<"OPAL latency capture->packet="<<t.capture_to_packet_ms<<"ms network="<<t.network_ms
        <<"ms reassembly="<<t.reassembly_ms<<"ms decode="<<t.decode_ms<<"ms present="<<t.present_ms
        <<"ms total="<<t.total_ms<<"ms loss="<<t.loss_percent<<"% stale="<<t.stale_frames
        <<" bitrate="<<t.bitrate_kbps<<"kbps decoder="<<t.decoder_backend
        <<" decode_fps="<<t.decoded_fps<<" present_fps="<<t.presented_fps;return out.str();
}

}
