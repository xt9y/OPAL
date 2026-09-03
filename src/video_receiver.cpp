#include <opal/video_receiver.hpp>

#include <opal/audio_output.hpp>
#include <opal/udp_transport.hpp>
#include <opal/video_crypto.hpp>
#include <opal/video_decoder.hpp>
#include <opal/video_feedback.hpp>
#include <opal/video_packet.hpp>
#include <opal/video_present.hpp>
#include <opal/video_reassembly.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace opal { namespace {
using Clock=std::chrono::steady_clock;
std::uint64_t monotonic_us(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());}
bool debug_enabled(){const char *v=std::getenv("OPAL_DEBUG");return v&&*v&&std::string(v)!="0";}
std::uint32_t read32(std::span<const std::uint8_t> bytes){return bytes.size()<4?0:(static_cast<std::uint32_t>(bytes[0])<<24)|(static_cast<std::uint32_t>(bytes[1])<<16)|(static_cast<std::uint32_t>(bytes[2])<<8)|bytes[3];}
double ewma(double old_value,double sample){return old_value==0.0?sample:old_value*0.8+sample*0.2;}
}}

struct VideoReceiver::Impl {
    DirectVideoPath path;
    std::function<void(const std::string&)> control_send;
    VideoReassembler reassembler;
    ReplayWindow1024 replay;
    VideoDecoder decoder;
    VideoPresenter presenter;
    AudioOutput audio_output;
    std::thread thread;
    std::atomic<bool> run{false},media{false};
    std::atomic<unsigned long> window{0};
    std::atomic<std::uint64_t> stale{0},highest{0};
    std::atomic<std::int64_t> latest_video_ts{0},clock_offset_us{0};
    std::atomic<std::uint32_t> current_rtt_us{0},last_decode_age_us{0};
    bool decoder_ready=false,audio_ready=false;
    Clock::time_point last_idr_request{},last_feedback{},last_clock{},last_debug{};
    std::uint64_t interval_first=0,interval_highest=0;std::uint32_t interval_received=0;
    std::map<std::uint64_t,std::uint64_t> frame_first_arrival;
    LatencyTelemetry telemetry;

    void request_idr(){
        const auto now=Clock::now();if(last_idr_request.time_since_epoch().count()!=0&&now-last_idr_request<std::chrono::milliseconds(250))return;
        last_idr_request=now;stale.fetch_add(1);audio_output.reset_to(latest_video_ts.load());
        if(control_send)control_send("REQUEST_IDR "+std::to_string(path.generation));
    }

    void update_telemetry(const ReassembledFrame &assembled,double reassembly_ms,double decode_ms,double present_ms,std::uint64_t first_arrival,std::uint64_t done_us){
        const auto offset=clock_offset_us.load();const double network=std::max(0.0,static_cast<double>(static_cast<std::int64_t>(first_arrival)-static_cast<std::int64_t>(assembled.capture_timestamp_us)+offset)/1000.0);
        const double total=std::max(0.0,static_cast<double>(static_cast<std::int64_t>(done_us)-static_cast<std::int64_t>(assembled.capture_timestamp_us)+offset)/1000.0);
        telemetry.network_ms=ewma(telemetry.network_ms,network);telemetry.reassembly_ms=ewma(telemetry.reassembly_ms,reassembly_ms);
        telemetry.decode_ms=ewma(telemetry.decode_ms,decode_ms);telemetry.present_ms=ewma(telemetry.present_ms,present_ms);telemetry.total_ms=ewma(telemetry.total_ms,total);
        telemetry.stale_frames=stale.load();last_decode_age_us.store(static_cast<std::uint32_t>(std::min(4294967295.0,total*1000.0)));
    }

    bool decode_and_present(const ReassembledFrame &assembled,double reassembly_ms,std::uint64_t first_arrival){
        const auto decode_begin=monotonic_us();std::vector<DecodedVideoFrame> frames;
        if(!decoder_ready||!decoder.decode(assembled.data,static_cast<std::int64_t>(assembled.capture_timestamp_us),frames)){
            for(auto &frame:frames)av_frame_free(&frame.frame);decoder.flush();request_idr();return false;
        }
        if(frames.empty())return true;
        for(std::size_t i=0;i+1<frames.size();++i){av_frame_free(&frames[i].frame);stale.fetch_add(1);}
        auto newest=frames.back();if(!newest.frame)return false;const auto decode_end=monotonic_us();
        if(!presenter.x11_window()){
            bool fullscreen=true;if(const char *windowed=std::getenv("OPAL_VIDEO_WINDOWED");windowed&&*windowed&&std::string(windowed)!="0")fullscreen=false;
            if(!presenter.open(newest.frame->width,newest.frame->height,fullscreen)){av_frame_free(&newest.frame);return false;}window.store(presenter.x11_window());
        }
        const auto present_begin=monotonic_us();if(!presenter.present(newest))return false;const auto present_end=monotonic_us();
        latest_video_ts.store(static_cast<std::int64_t>(assembled.capture_timestamp_us));media.store(true);
        update_telemetry(assembled,reassembly_ms,static_cast<double>(decode_end-decode_begin)/1000.0,static_cast<double>(present_end-present_begin)/1000.0,first_arrival,present_end);return true;
    }

    void handle_complete(const ReassembledFrame &assembled,double reassembly_ms,std::uint64_t first_arrival){
        if(assembled.media_type==VideoMediaType::VideoH264){
            if(assembled.config){decoder.flush();decoder_ready=decoder.configure_h264(assembled.data);if(!decoder_ready)request_idr();return;}
            decode_and_present(assembled,reassembly_ms,first_arrival);return;
        }
        if(assembled.media_type==VideoMediaType::AudioAac){
            if(assembled.config){
                if(assembled.data.size()<5){audio_ready=false;return;}const int rate=static_cast<int>(read32(assembled.data));const int channels=assembled.data[4];
                audio_ready=audio_output.configure_aac(std::span<const std::uint8_t>(assembled.data).subspan(5),rate,channels);return;
            }
            if(audio_ready)audio_output.submit(assembled.data,static_cast<std::int64_t>(assembled.capture_timestamp_us),latest_video_ts.load());
        }
    }

    void note_sequence(std::uint64_t sequence){
        if(interval_first==0){interval_first=sequence;interval_highest=sequence;interval_received=1;}else{interval_highest=std::max(interval_highest,sequence);++interval_received;}
        auto previous=highest.load();while(sequence>previous&&!highest.compare_exchange_weak(previous,sequence)){}
    }

    void control_tick(){
        const auto now=Clock::now();
        if(last_feedback.time_since_epoch().count()==0||now-last_feedback>=std::chrono::milliseconds(100)){
            VideoFeedbackSample sample;sample.highest_sequence=highest.load();sample.received=interval_received;
            if(interval_first&&interval_highest>=interval_first){const auto expected=interval_highest-interval_first+1;sample.lost=static_cast<std::uint32_t>(std::min<std::uint64_t>(0xffffffffULL,expected>interval_received?expected-interval_received:0));}
            sample.rtt_us=current_rtt_us.load();sample.decode_age_us=last_decode_age_us.load();if(control_send)control_send(video_feedback_line(path.generation,sample));
            const auto total=static_cast<std::uint64_t>(sample.received)+sample.lost;telemetry.loss_percent=total?100.0*sample.lost/static_cast<double>(total):0.0;
            interval_first=interval_highest=0;interval_received=0;last_feedback=now;
        }
        if(last_clock.time_since_epoch().count()==0||now-last_clock>=std::chrono::seconds(1)){if(control_send)control_send(clock_sync_request_line(path.generation,static_cast<std::int64_t>(monotonic_us())));last_clock=now;}
        if(debug_enabled()&&(last_debug.time_since_epoch().count()==0||now-last_debug>=std::chrono::seconds(1))){telemetry.stale_frames=stale.load();std::cerr<<format_latency_telemetry(telemetry)<<" audio="<<audio_output.queued_ms()<<"ms\n";last_debug=now;}
    }

    bool handle_control(const std::string &line){
        std::int64_t t0=0,t1=0,t2=0;if(!parse_clock_sync_line(line,path.generation,t0,t1,t2)||t1==0||t2==0)return false;
        auto estimate=estimate_clock_offset(t0,t1,t2,static_cast<std::int64_t>(monotonic_us()));if(!estimate.valid)return true;
        clock_offset_us.store(estimate.offset_us);current_rtt_us.store(estimate.rtt_us);return true;
    }

    void loop(){
        reassembler.reset(path.generation,path.session_id);replay.reset();std::array<std::uint8_t,kVideoMaxDatagramBytes+1> wire{};
        while(run.load()){
            sockaddr_storage source{};socklen_t source_len=sizeof(source);const int received=recv_datagram(path.socket.fd,wire,source,source_len,20);
            if(received>0&&received<=static_cast<int>(kVideoMaxDatagramBytes)){
                const auto arrival=monotonic_us();const auto bytes=std::span<const std::uint8_t>(wire.data(),static_cast<std::size_t>(received));VideoPacketHeader header;
                if(parse_video_header(bytes,header)&&header.generation==path.generation&&header.session_id==path.session_id&&header.media_type!=VideoMediaType::Probe&&header.media_type!=VideoMediaType::ProbeAck&&
                   bytes.size()==kVideoHeaderBytes+static_cast<std::size_t>(header.payload_length)+kVideoAeadTagBytes){
                    std::vector<std::uint8_t> plaintext;
                    if(open_video_datagram(path.keys,header.packet_sequence,bytes.first(kVideoHeaderBytes),bytes.subspan(kVideoHeaderBytes),plaintext)&&plaintext.size()==header.payload_length&&replay.accept(header.packet_sequence)){
                        note_sequence(header.packet_sequence);if(!frame_first_arrival.contains(header.frame_id))frame_first_arrival[header.frame_id]=arrival;
                        while(frame_first_arrival.size()>8)frame_first_arrival.erase(frame_first_arrival.begin());
                        VideoPlainPacket packet{header,std::move(plaintext)};ReassembledFrame assembled;const auto status=reassembler.accept(packet,assembled);
                        if(status==ReassemblyStatus::NeedIdr)request_idr();
                        else if(status==ReassemblyStatus::Complete){auto it=frame_first_arrival.find(assembled.frame_id);const auto first=it==frame_first_arrival.end()?arrival:it->second;if(it!=frame_first_arrival.end())frame_first_arrival.erase(it);handle_complete(assembled,static_cast<double>(arrival-first)/1000.0,first);}
                    }
                }
            }
            control_tick();
        }
        audio_output.close();presenter.close();window.store(0);decoder.flush();
    }
};

VideoReceiver::VideoReceiver():impl_(std::make_unique<Impl>()){}
bool VideoReceiver::start(DirectVideoPath path,std::function<void(const std::string&)> control_send){
    stop();impl_=std::make_unique<Impl>();if(path.socket.fd<0||path.peer_len==0||path.session_id==0||path.generation==0)return false;
    impl_->path=std::move(path);impl_->control_send=std::move(control_send);impl_->run.store(true);impl_->thread=std::thread([this]{impl_->loop();});return true;
}
bool VideoReceiver::handle_control_line(const std::string &line){return impl_&&impl_->handle_control(line);}
bool VideoReceiver::media_started() const{return impl_&&impl_->media.load();}
Window VideoReceiver::presentation_window() const{return impl_?static_cast<Window>(impl_->window.load()):0;}
std::uint64_t VideoReceiver::stale_frames() const{return impl_?impl_->stale.load():0;}
std::uint64_t VideoReceiver::highest_sequence() const{return impl_?impl_->highest.load():0;}
std::uint32_t VideoReceiver::audio_queued_ms() const{return impl_?impl_->audio_output.queued_ms():0;}
void VideoReceiver::stop(){if(!impl_)return;impl_->run.store(false);if(impl_->thread.joinable())impl_->thread.join();}
VideoReceiver::~VideoReceiver(){stop();}

}
