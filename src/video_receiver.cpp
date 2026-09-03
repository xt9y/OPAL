#include <opal/video_receiver.hpp>

#include <opal/udp_transport.hpp>
#include <opal/video_crypto.hpp>
#include <opal/video_decoder.hpp>
#include <opal/video_packet.hpp>
#include <opal/video_present.hpp>
#include <opal/video_reassembly.hpp>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <utility>
#include <vector>

namespace opal {

struct VideoReceiver::Impl {
    DirectVideoPath path;
    std::function<void(const std::string&)> control_send;
    VideoReassembler reassembler;
    ReplayWindow1024 replay;
    VideoDecoder decoder;
    VideoPresenter presenter;
    std::thread thread;
    std::atomic<bool> run{false},media{false};
    std::atomic<unsigned long> window{0};
    std::atomic<std::uint64_t> stale{0},highest{0};
    bool decoder_ready=false;
    std::chrono::steady_clock::time_point last_idr_request{};

    void request_idr(){
        const auto now=std::chrono::steady_clock::now();
        if(last_idr_request.time_since_epoch().count()!=0&&now-last_idr_request<std::chrono::milliseconds(250))return;
        last_idr_request=now;stale.fetch_add(1);
        if(control_send)control_send("REQUEST_IDR "+std::to_string(path.generation));
    }

    bool decode_and_present(const ReassembledFrame &assembled){
        std::vector<DecodedVideoFrame> frames;
        if(!decoder_ready||!decoder.decode(assembled.data,static_cast<std::int64_t>(assembled.capture_timestamp_us),frames)){
            for(auto &frame:frames)av_frame_free(&frame.frame);
            decoder.flush();request_idr();return false;
        }
        if(frames.empty())return true;
        for(std::size_t i=0;i+1<frames.size();++i){av_frame_free(&frames[i].frame);stale.fetch_add(1);}
        auto newest=frames.back();
        if(!newest.frame)return false;
        if(!presenter.x11_window()){
            bool fullscreen=true;
            if(const char *windowed=std::getenv("OPAL_VIDEO_WINDOWED");windowed&&*windowed&&std::string(windowed)!="0")fullscreen=false;
            if(!presenter.open(newest.frame->width,newest.frame->height,fullscreen)){av_frame_free(&newest.frame);return false;}
            window.store(presenter.x11_window());
        }
        if(!presenter.present(newest))return false;
        media.store(true);return true;
    }

    void handle_complete(const ReassembledFrame &assembled){
        if(assembled.media_type==VideoMediaType::VideoH264){
            if(assembled.config){
                decoder.flush();decoder_ready=decoder.configure_h264(assembled.data);
                if(!decoder_ready)request_idr();
                return;
            }
            decode_and_present(assembled);
        }
    }

    void loop(){
        reassembler.reset(path.generation,path.session_id);replay.reset();
        std::array<std::uint8_t,kVideoMaxDatagramBytes+1> wire{};
        while(run.load()){
            sockaddr_storage source{};socklen_t source_len=sizeof(source);
            const int received=recv_datagram(path.socket.fd,wire,source,source_len,20);
            if(received<=0)continue;
            if(received>static_cast<int>(kVideoMaxDatagramBytes))continue;
            const auto bytes=std::span<const std::uint8_t>(wire.data(),static_cast<std::size_t>(received));
            VideoPacketHeader header;
            if(!parse_video_header(bytes,header)||header.generation!=path.generation||header.session_id!=path.session_id)continue;
            if(header.media_type==VideoMediaType::Probe||header.media_type==VideoMediaType::ProbeAck)continue;
            if(bytes.size()!=kVideoHeaderBytes+static_cast<std::size_t>(header.payload_length)+kVideoAeadTagBytes)continue;
            std::vector<std::uint8_t> plaintext;
            if(!open_video_datagram(path.keys,header.packet_sequence,bytes.first(kVideoHeaderBytes),bytes.subspan(kVideoHeaderBytes),plaintext))continue;
            if(plaintext.size()!=header.payload_length||!replay.accept(header.packet_sequence))continue;
            auto previous=highest.load();while(header.packet_sequence>previous&&!highest.compare_exchange_weak(previous,header.packet_sequence)){}
            VideoPlainPacket packet{header,std::move(plaintext)};ReassembledFrame assembled;
            const auto status=reassembler.accept(packet,assembled);
            if(status==ReassemblyStatus::NeedIdr)request_idr();
            else if(status==ReassemblyStatus::Complete)handle_complete(assembled);
        }
        presenter.close();window.store(0);decoder.flush();
    }
};

VideoReceiver::VideoReceiver():impl_(std::make_unique<Impl>()){}

bool VideoReceiver::start(DirectVideoPath path,std::function<void(const std::string&)> control_send){
    stop();impl_=std::make_unique<Impl>();
    if(path.socket.fd<0||path.peer_len==0||path.session_id==0||path.generation==0)return false;
    impl_->path=std::move(path);impl_->control_send=std::move(control_send);impl_->run.store(true);
    impl_->thread=std::thread([this]{impl_->loop();});return true;
}

bool VideoReceiver::media_started() const{return impl_&&impl_->media.load();}
Window VideoReceiver::presentation_window() const{return impl_?static_cast<Window>(impl_->window.load()):0;}
std::uint64_t VideoReceiver::stale_frames() const{return impl_?impl_->stale.load():0;}
std::uint64_t VideoReceiver::highest_sequence() const{return impl_?impl_->highest.load():0;}

void VideoReceiver::stop(){
    if(!impl_)return;
    impl_->run.store(false);
    if(impl_->thread.joinable())impl_->thread.join();
}

VideoReceiver::~VideoReceiver(){stop();}

}
