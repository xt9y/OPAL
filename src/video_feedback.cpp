#include <opal/video_feedback.hpp>

#include <algorithm>
#include <iomanip>
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
        if(++bad_samples_>=2){target_=std::max(floor_,target_*80/100);bad_samples_=0;}
        return target_;
    }
    bad_samples_=0;
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
    ClockEstimate out;if(t1<t0-60000000LL||t2<t1||t3<t0)return out;
    const std::int64_t rtt=(t3-t0)-(t2-t1);if(rtt<0||rtt>60000000LL)return out;
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
    if(!(in>>word>>gen>>t0>>t1>>t2)||in>>extra||word!="CLOCK_SYNC"||gen!=generation)return false;return true;
}

std::string format_latency_telemetry(const LatencyTelemetry &t){
    std::ostringstream out;out.setf(std::ios::fixed);out<<std::setprecision(1)
        <<"OPAL latency capture->packet="<<t.capture_to_packet_ms<<"ms network="<<t.network_ms
        <<"ms reassembly="<<t.reassembly_ms<<"ms decode="<<t.decode_ms<<"ms present="<<t.present_ms
        <<"ms total="<<t.total_ms<<"ms loss="<<t.loss_percent<<"% stale="<<t.stale_frames
        <<" bitrate="<<t.bitrate_kbps<<"kbps";return out.str();
}

}
