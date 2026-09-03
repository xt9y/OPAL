#include <opal/video_sender.hpp>

#include <opal/config.hpp>
#include <opal/media_profile.hpp>
#include <opal/udp_transport.hpp>
#include <opal/video_capture.hpp>
#include <opal/video_crypto.hpp>
#include <opal/video_packet.hpp>
#include <algorithm>
#include <chrono>
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

bool send_encrypted(const DirectVideoPath &path,const VideoPlainPacket &packet){
    const auto aad=serialize_video_header(packet.header);
    std::vector<std::uint8_t> sealed;
    if(!seal_video_datagram(path.keys,packet.header.packet_sequence,aad,packet.payload,sealed))return false;
    std::vector<std::uint8_t> wire;wire.reserve(aad.size()+sealed.size());
    wire.insert(wire.end(),aad.begin(),aad.end());wire.insert(wire.end(),sealed.begin(),sealed.end());
    if(wire.size()>kVideoMaxDatagramBytes)return false;
    return send_datagram(path.socket.fd,path.peer,path.peer_len,wire);
}
}

struct VideoSender::Impl {
    DirectVideoPath path;
    StreamOptions stream;
    bool audio=false;
    std::function<void(const std::string&)> control_send;
    VideoCapture capture;
    std::thread thread;
    std::atomic<bool> run{false},idr_requested{false};
    std::atomic<int> bitrate_kbps{30000};
    std::atomic<std::uint64_t> stale{0};
    std::uint64_t packet_sequence=kFirstMediaSequence,frame_id=1;
    Clock::time_point last_restart{};
    std::string portal_token;

    bool start_capture(){
        const int bitrate=std::max(1000,bitrate_kbps.load());
        if(!capture.start(stream,bitrate,audio,portal_token))return false;
        last_restart=Clock::now();
        return true;
    }

    bool send_frame(VideoMediaType type,std::uint16_t flags,std::span<const std::uint8_t> data){
        auto packets=fragment_media_unit(type,flags,path.generation,path.session_id,frame_id++,monotonic_us(),data,packet_sequence,true);
        if(packets.empty())return false;
        int failures=0;
        for(const auto &packet:packets)if(!send_encrypted(path,packet))++failures;
        if(failures>1){
            if(type==VideoMediaType::VideoH264&&(flags&FrameKeyframe)==0)stale.fetch_add(1);
            else idr_requested.store(true);
            return false;
        }
        return true;
    }

    void send_configs(){
        for(const auto &config:capture.configs()){
            if(config.extradata.empty())continue;
            send_frame(config.kind==MediaKind::VideoH264?VideoMediaType::VideoH264:VideoMediaType::AudioAac,
                       FrameConfig,config.extradata);
        }
    }

    bool maybe_restart(){
        if(!idr_requested.load())return true;
        const auto now=Clock::now();
        if(last_restart.time_since_epoch().count()!=0&&now-last_restart<std::chrono::milliseconds(250))return true;
        idr_requested.store(false);
        capture.stop();
        if(!start_capture())return false;
        send_configs();
        return true;
    }

    void loop(){
        send_configs();
        while(run.load()){
            if(!maybe_restart()){run.store(false);break;}
            EncodedMediaUnit unit;
            if(!capture.next(unit,100)){std::this_thread::sleep_for(std::chrono::milliseconds(1));continue;}
            const auto type=unit.kind==MediaKind::VideoH264?VideoMediaType::VideoH264:VideoMediaType::AudioAac;
            std::uint16_t flags=unit.keyframe?FrameKeyframe:0;
            send_frame(type,flags,unit.data);
        }
        capture.stop();
    }
};

VideoSender::VideoSender():impl_(std::make_unique<Impl>()){}

bool VideoSender::start(DirectVideoPath path,const StreamOptions &stream,bool audio,
                        std::function<void(const std::string&)> control_send){
    stop();impl_=std::make_unique<Impl>();
    if(path.socket.fd<0||path.peer_len==0||path.session_id==0||path.generation==0)return false;
    impl_->path=std::move(path);impl_->stream=stream;impl_->audio=audio;impl_->control_send=std::move(control_send);
    impl_->bitrate_kbps.store(automatic_bitrate_kbps(stream.max_width,stream.max_height,stream.fps));
    impl_->portal_token=(Paths::load().root/"portal-session.token").string();
    if(!impl_->start_capture())return false;
    impl_->run.store(true);impl_->thread=std::thread([this]{impl_->loop();});return true;
}

void VideoSender::request_idr(){if(impl_)impl_->idr_requested.store(true);}
void VideoSender::set_target_bitrate(int kbps){if(impl_)impl_->bitrate_kbps.store(std::max(1000,kbps));}
int VideoSender::target_bitrate() const{return impl_?impl_->bitrate_kbps.load():0;}
std::size_t VideoSender::queued_frames() const{return 0;}
std::size_t VideoSender::queued_bytes() const{return 0;}
std::uint64_t VideoSender::stale_frames() const{return impl_?impl_->stale.load():0;}

void VideoSender::stop(){
    if(!impl_)return;
    impl_->run.store(false);impl_->capture.stop();
    if(impl_->thread.joinable())impl_->thread.join();
}

VideoSender::~VideoSender(){stop();}

}
