#include <opal/video_sender.hpp>

#include <opal/config.hpp>
#include <opal/media_profile.hpp>
#include <opal/udp_transport.hpp>
#include <opal/video_capture.hpp>
#include <opal/video_crypto.hpp>
#include <opal/video_feedback.hpp>
#include <opal/video_packet.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace opal { namespace {
using Clock=std::chrono::steady_clock;
constexpr std::uint64_t kFirstMediaSequence=1ULL<<32;

std::uint64_t monotonic_us(){
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now().time_since_epoch()).count());
}

bool debug_enabled(){const char *v=std::getenv("OPAL_DEBUG");return v&&*v&&std::string(v)!="0";}

std::vector<std::uint8_t> encrypt_packet(const DirectVideoPath &path,const VideoPlainPacket &packet){
    const auto aad=serialize_video_header(packet.header);std::vector<std::uint8_t> sealed;
    if(!seal_video_datagram(path.keys,packet.header.packet_sequence,aad,packet.payload,sealed))return {};
    std::vector<std::uint8_t> wire;wire.reserve(aad.size()+sealed.size());wire.insert(wire.end(),aad.begin(),aad.end());wire.insert(wire.end(),sealed.begin(),sealed.end());
    if(wire.size()>kVideoMaxDatagramBytes)return {};return wire;
}

void put32(std::vector<std::uint8_t> &out,std::uint32_t value){out.push_back(static_cast<std::uint8_t>(value>>24));out.push_back(static_cast<std::uint8_t>(value>>16));out.push_back(static_cast<std::uint8_t>(value>>8));out.push_back(static_cast<std::uint8_t>(value));}
}

struct VideoSender::Impl {
    DirectVideoPath path;
    StreamOptions stream;
    bool audio=false;
    std::function<void(const std::string&)> control_send;
    VideoCapture capture;
    std::thread thread;
    std::atomic<bool> run{false},idr_requested{false};
    std::atomic<int> target_kbps{30000},active_kbps{30000};
    std::atomic<std::uint64_t> stale{0};
    std::unique_ptr<BitrateController> controller;
    std::mutex feedback_mu;
    std::uint64_t packet_sequence=kFirstMediaSequence,frame_id=1;
    Clock::time_point last_restart{},token_time{},last_debug{};
    double tokens=0.0,capture_to_packet_ewma=0.0;
    std::string portal_token;
    int test_drop_fragments=0;bool test_drop_done=false;

    bool start_capture(){
        const int bitrate=std::max(1000,target_kbps.load());
        if(!capture.start(stream,bitrate,audio,portal_token))return false;
        active_kbps.store(bitrate);last_restart=Clock::now();tokens=2.0*kVideoMaxDatagramBytes;token_time=last_restart;return true;
    }

    void refill_tokens(){
        const auto now=Clock::now();if(token_time.time_since_epoch().count()==0)token_time=now;
        const double us=static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(now-token_time).count());token_time=now;
        const double bytes_per_us=std::max(0.001,static_cast<double>(target_kbps.load())*1.20/8000.0);
        tokens=std::min(2.0*kVideoMaxDatagramBytes,tokens+us*bytes_per_us);
    }

    bool paced_send(const std::vector<std::uint8_t> &wire,Clock::time_point deadline,bool droppable){
        if(wire.empty()||wire.size()>kVideoMaxDatagramBytes)return false;
        for(;;){
            refill_tokens();if(tokens>=wire.size())break;
            const double bytes_per_us=std::max(0.001,static_cast<double>(target_kbps.load())*1.20/8000.0);
            const auto wait_us=static_cast<long long>(std::ceil((wire.size()-tokens)/bytes_per_us));
            if(droppable&&Clock::now()+std::chrono::microseconds(wait_us)>deadline)return false;
            std::this_thread::sleep_for(std::chrono::microseconds(std::clamp<long long>(wait_us,50,1000)));
            if(!run.load())return false;
        }
        tokens-=wire.size();return send_datagram(path.socket.fd,path.peer,path.peer_len,wire);
    }

    bool send_frame(VideoMediaType type,std::uint16_t flags,std::span<const std::uint8_t> data,std::uint64_t capture_time=0){
        const auto start=Clock::now();const bool ordinary=type==VideoMediaType::VideoH264&&(flags&(FrameKeyframe|FrameConfig))==0;
        const bool audio_frame=type==VideoMediaType::AudioAac&&(flags&FrameConfig)==0;
        const auto deadline=start+(ordinary?std::chrono::microseconds(1000000/std::max(15,stream.fps)):(audio_frame?std::chrono::milliseconds(10):std::chrono::milliseconds(250)));
        auto packets=fragment_media_unit(type,flags,path.generation,path.session_id,frame_id++,capture_time?capture_time:monotonic_us(),data,packet_sequence,true);
        if(packets.empty())return false;
        int failures=0;int deliberately_dropped=0;
        const bool inject_drop=!test_drop_done&&type==VideoMediaType::VideoH264&&(flags&FrameConfig)==0&&test_drop_fragments>0;
        for(const auto &packet:packets){
            if(inject_drop&&packet.header.media_type!=VideoMediaType::Fec&&deliberately_dropped<test_drop_fragments){++deliberately_dropped;continue;}
            auto wire=encrypt_packet(path,packet);
            if(!paced_send(wire,deadline,ordinary||audio_frame)){
                ++failures;
                if(ordinary||audio_frame)break;
            }
        }
        if(inject_drop)test_drop_done=true;
        if(failures){
            if(ordinary||audio_frame)stale.fetch_add(1);else idr_requested.store(true);
            return false;
        }
        return true;
    }

    void send_configs(){
        for(const auto &config:capture.configs()){
            if(config.kind==MediaKind::VideoH264){if(!config.extradata.empty())send_frame(VideoMediaType::VideoH264,FrameConfig,config.extradata);continue;}
            if(config.sample_rate<=0||config.channels<=0||config.channels>255)continue;
            std::vector<std::uint8_t> payload;payload.reserve(5+config.extradata.size());put32(payload,static_cast<std::uint32_t>(config.sample_rate));payload.push_back(static_cast<std::uint8_t>(config.channels));payload.insert(payload.end(),config.extradata.begin(),config.extradata.end());
            send_frame(VideoMediaType::AudioAac,FrameConfig,payload);
        }
    }

    bool maybe_restart(){
        const auto now=Clock::now();const int target=target_kbps.load(),active=std::max(1,active_kbps.load());
        const bool bitrate_change=std::abs(target-active)*100>=active*15&&now-last_restart>=std::chrono::seconds(2);
        const bool idr_change=idr_requested.load()&&now-last_restart>=std::chrono::milliseconds(250);
        if(!bitrate_change&&!idr_change)return true;
        idr_requested.store(false);capture.stop();if(!start_capture())return false;send_configs();return true;
    }

    void debug_sample(const EncodedMediaUnit &unit){
        if(!unit.capture_time_us)return;const double sample=static_cast<double>(monotonic_us()-unit.capture_time_us)/1000.0;
        capture_to_packet_ewma=capture_to_packet_ewma==0.0?sample:capture_to_packet_ewma*0.8+sample*0.2;
        const auto now=Clock::now();if(debug_enabled()&&(last_debug.time_since_epoch().count()==0||now-last_debug>=std::chrono::seconds(1))){
            last_debug=now;std::cerr<<"OPAL latency capture->packet="<<capture_to_packet_ewma<<"ms bitrate="<<active_kbps.load()<<"kbps stale="<<stale.load()<<"\n";
        }
    }

    void loop(){
        send_configs();
        while(run.load()){
            if(!maybe_restart()){run.store(false);break;}
            EncodedMediaUnit unit;if(!capture.next(unit,100)){std::this_thread::sleep_for(std::chrono::milliseconds(1));continue;}
            const auto type=unit.kind==MediaKind::VideoH264?VideoMediaType::VideoH264:VideoMediaType::AudioAac;
            const std::uint16_t flags=unit.keyframe?FrameKeyframe:0;send_frame(type,flags,unit.data,unit.capture_time_us);debug_sample(unit);
        }
        capture.stop();
    }

    bool handle_control(const std::string &line){
        {std::istringstream in(line);std::string word,extra;unsigned long long generation=0;if((in>>word>>generation)&&word=="REQUEST_IDR"&&generation==path.generation&&!(in>>extra)){idr_requested.store(true);return true;}}
        VideoFeedbackSample sample;if(parse_video_feedback_line(line,path.generation,sample)){
            std::lock_guard<std::mutex> lock(feedback_mu);if(controller)target_kbps.store(controller->on_feedback(sample,Clock::now()));return true;
        }
        std::int64_t t0=0,t1=0,t2=0;if(parse_clock_sync_line(line,path.generation,t0,t1,t2)&&t1==0&&t2==0){
            const auto receive=static_cast<std::int64_t>(monotonic_us());const auto send=static_cast<std::int64_t>(monotonic_us());if(control_send)control_send(clock_sync_reply_line(path.generation,t0,receive,send));return true;
        }
        return false;
    }
};

VideoSender::VideoSender():impl_(std::make_unique<Impl>()){}

bool VideoSender::start(DirectVideoPath path,const StreamOptions &stream,bool audio,std::function<void(const std::string&)> control_send){
    stop();impl_=std::make_unique<Impl>();if(path.socket.fd<0||path.peer_len==0||path.session_id==0||path.generation==0)return false;
    impl_->path=std::move(path);impl_->stream=stream;impl_->audio=audio;impl_->control_send=std::move(control_send);
    const int ceiling=automatic_bitrate_kbps(stream.max_width,stream.max_height,stream.fps);impl_->controller=std::make_unique<BitrateController>(ceiling);impl_->target_kbps.store(ceiling);impl_->active_kbps.store(ceiling);
    impl_->portal_token=(Paths::load().root/"portal-session.token").string();
    if(const char *drop=std::getenv("OPAL_TEST_DROP_FRAGMENTS");drop&&*drop)try{impl_->test_drop_fragments=std::clamp(std::stoi(drop),0,10);}catch(...){impl_->test_drop_fragments=0;}
    if(!impl_->start_capture())return false;impl_->run.store(true);impl_->thread=std::thread([this]{impl_->loop();});return true;
}

void VideoSender::request_idr(){if(impl_)impl_->idr_requested.store(true);}
bool VideoSender::handle_control_line(const std::string &line){return impl_&&impl_->handle_control(line);}
void VideoSender::set_target_bitrate(int kbps){if(impl_)impl_->target_kbps.store(std::max(1000,kbps));}
int VideoSender::target_bitrate() const{return impl_?impl_->target_kbps.load():0;}
std::size_t VideoSender::queued_frames() const{return 0;}
std::size_t VideoSender::queued_bytes() const{return 0;}
std::uint64_t VideoSender::stale_frames() const{return impl_?impl_->stale.load():0;}

void VideoSender::stop(){if(!impl_)return;impl_->run.store(false);if(impl_->thread.joinable())impl_->thread.join();}
VideoSender::~VideoSender(){stop();}

}
