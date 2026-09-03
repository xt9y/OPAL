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
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>

namespace opal { namespace {
using Clock=std::chrono::steady_clock;
std::uint64_t monotonic_us(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());}
bool debug_enabled(){const char *v=std::getenv("OPAL_DEBUG");return v&&*v&&std::string(v)!="0";}
std::uint32_t read32(std::span<const std::uint8_t> b){return b.size()<4?0:(static_cast<std::uint32_t>(b[0])<<24)|(static_cast<std::uint32_t>(b[1])<<16)|(static_cast<std::uint32_t>(b[2])<<8)|b[3];}
double ewma(double old,double sample){return old==0.0?sample:old*0.8+sample*0.2;}
}

struct VideoReceiver::Impl{
    struct Arrival{std::uint64_t id=0,us=0;};
    DirectVideoPath path;std::function<void(const std::string&)>control_send;VideoReassembler reassembler;ReplayWindow1024 replay;std::unique_ptr<VideoCipher>cipher;std::array<std::uint8_t,kVideoPlaintextBytes>plaintext_buffer{};VideoDecoder decoder;VideoPresenter presenter;AudioOutput audio_output;ReassembledFrame assembled;std::array<Arrival,8> arrivals{};std::size_t arrival_cursor=0;std::thread thread;std::atomic<bool>run{false},media{false},clock_valid{false};std::atomic<VideoReceiverFailure>failure{VideoReceiverFailure::NoFailure};std::atomic<unsigned long>window{0};std::atomic<std::uint64_t>stale{0},highest{0};std::atomic<std::int64_t>latest_video_ts{0},clock_offset_us{0};std::atomic<std::uint32_t>current_rtt_us{0},last_decode_age_us{0},host_capture_to_packet_us{0};std::atomic<int>host_active_kbps{0};bool decoder_ready=false,audio_ready=false;Clock::time_point last_idr_request{},last_feedback{},last_clock{},last_debug{},last_media_packet{};std::uint64_t interval_first=0,interval_highest=0,feedback_cursor=0;std::uint32_t interval_received=0;LatencyTelemetry telemetry;
    void fail(VideoReceiverFailure reason){auto expected=VideoReceiverFailure::NoFailure;failure.compare_exchange_strong(expected,reason);run.store(false);}
    void note_arrival(std::uint64_t id,std::uint64_t us){for(const auto&s:arrivals)if(s.id==id)return;for(auto&s:arrivals)if(s.id==0){s={id,us};return;}arrivals[arrival_cursor%arrivals.size()]={id,us};arrival_cursor=(arrival_cursor+1)%arrivals.size();}
    std::uint64_t take_arrival(std::uint64_t id,std::uint64_t fallback){for(auto&s:arrivals)if(s.id==id){const auto us=s.us;s={};return us;}return fallback;}
    void request_idr(){reassembler.require_idr();const auto now=Clock::now();if(last_idr_request.time_since_epoch().count()!=0&&now-last_idr_request<std::chrono::milliseconds(250))return;last_idr_request=now;stale.fetch_add(1);audio_output.reset_to(latest_video_ts.load());if(control_send)control_send("REQUEST_IDR "+std::to_string(path.generation));}
    void update_telemetry(const ReassembledFrame&a,double reassembly_ms,double decode_ms,double present_ms,std::uint64_t first,std::uint64_t done){telemetry.reassembly_ms=ewma(telemetry.reassembly_ms,reassembly_ms);telemetry.decode_ms=ewma(telemetry.decode_ms,decode_ms);telemetry.present_ms=ewma(telemetry.present_ms,present_ms);if(clock_valid.load(std::memory_order_acquire)){const auto offset=clock_offset_us.load();const double network=std::max(0.0,static_cast<double>(static_cast<std::int64_t>(first)-static_cast<std::int64_t>(a.capture_timestamp_us)+offset)/1000.0),total=std::max(0.0,static_cast<double>(static_cast<std::int64_t>(done)-static_cast<std::int64_t>(a.capture_timestamp_us)+offset)/1000.0);telemetry.network_ms=ewma(telemetry.network_ms,network);telemetry.total_ms=ewma(telemetry.total_ms,total);last_decode_age_us.store(static_cast<std::uint32_t>(std::min(4294967295.0,total*1000.0)));}else last_decode_age_us.store(0);telemetry.stale_frames=stale.load();}
    bool decode_and_present(const ReassembledFrame&a,double reassembly_ms,std::uint64_t first){const auto decode_begin=monotonic_us();DecodedVideoView newest;std::size_t superseded=0;if(!decoder_ready||!decoder.decode_latest(a.data,static_cast<std::int64_t>(a.capture_timestamp_us),newest,superseded)){decoder.flush();request_idr();return false;}if(!newest.frame)return true;if(superseded)stale.fetch_add(superseded);const auto decode_end=monotonic_us();if(!presenter.x11_window()){bool fullscreen=true;if(const char*w=std::getenv("OPAL_VIDEO_WINDOWED");w&&*w&&std::string(w)!="0")fullscreen=false;if(!presenter.open(newest.frame->width,newest.frame->height,fullscreen)){fail(VideoReceiverFailure::PresenterOpen);return false;}window.store(presenter.x11_window());}const auto present_begin=monotonic_us();if(!presenter.present_borrowed(newest)){fail(VideoReceiverFailure::Present);return false;}const auto present_end=monotonic_us();latest_video_ts.store(static_cast<std::int64_t>(a.capture_timestamp_us));media.store(true);update_telemetry(a,reassembly_ms,static_cast<double>(decode_end-decode_begin)/1000.0,static_cast<double>(present_end-present_begin)/1000.0,first,present_end);return true;}
    void handle_complete(const ReassembledFrame&a,double reassembly_ms,std::uint64_t first){if(a.media_type==VideoMediaType::VideoH264){if(a.config){decoder.flush();decoder_ready=decoder.configure_h264(a.data);reassembler.require_idr();if(!decoder_ready)request_idr();return;}decode_and_present(a,reassembly_ms,first);return;}if(a.media_type==VideoMediaType::AudioAac){if(a.config){if(a.data.size()<5){audio_ready=false;return;}const int rate=static_cast<int>(read32(a.data)),channels=a.data[4];audio_ready=audio_output.configure_aac(std::span<const std::uint8_t>(a.data).subspan(5),rate,channels);return;}if(audio_ready)audio_output.submit(a.data,static_cast<std::int64_t>(a.capture_timestamp_us),latest_video_ts.load());}}
    void note_sequence(std::uint64_t seq){auto prev=highest.load();while(seq>prev&&!highest.compare_exchange_weak(prev,seq)){}if(feedback_cursor&&seq<feedback_cursor)return;if(interval_received==0){interval_first=feedback_cursor?feedback_cursor:seq;interval_highest=seq;interval_received=1;}else{interval_highest=std::max(interval_highest,seq);++interval_received;}}
    void control_tick(){const auto now=Clock::now();telemetry.capture_to_packet_ms=static_cast<double>(host_capture_to_packet_us.load(std::memory_order_relaxed))/1000.0;telemetry.bitrate_kbps=host_active_kbps.load(std::memory_order_relaxed);if(last_feedback.time_since_epoch().count()==0||now-last_feedback>=std::chrono::milliseconds(100)){VideoFeedbackSample s;s.highest_sequence=highest.load();s.received=interval_received;if(interval_received&&interval_first&&interval_highest>=interval_first){const auto expected=interval_highest-interval_first+1;s.lost=static_cast<std::uint32_t>(std::min<std::uint64_t>(0xffffffffULL,expected>interval_received?expected-interval_received:0));feedback_cursor=interval_highest+1;}s.rtt_us=current_rtt_us.load();s.decode_age_us=last_decode_age_us.load();if(control_send)control_send(video_feedback_line(path.generation,s));const auto total=static_cast<std::uint64_t>(s.received)+s.lost;telemetry.loss_percent=total?100.0*s.lost/static_cast<double>(total):0.0;interval_first=interval_highest=0;interval_received=0;last_feedback=now;}if(last_clock.time_since_epoch().count()==0||now-last_clock>=std::chrono::seconds(1)){if(control_send)control_send(clock_sync_request_line(path.generation,static_cast<std::int64_t>(monotonic_us())));last_clock=now;}if(debug_enabled()&&(last_debug.time_since_epoch().count()==0||now-last_debug>=std::chrono::seconds(1))){telemetry.stale_frames=stale.load();std::cerr<<format_latency_telemetry(telemetry)<<" audio="<<audio_output.queued_ms()<<"ms\n";last_debug=now;}}
    bool handle_control(const std::string&line){HostMediaDebugSample host_debug;if(parse_host_media_debug_line(line,path.generation,host_debug)){host_capture_to_packet_us.store(host_debug.capture_to_packet_us,std::memory_order_relaxed);host_active_kbps.store(host_debug.active_kbps,std::memory_order_relaxed);if(debug_enabled())std::cerr<<format_host_media_debug(host_debug)<<"\n";return true;}std::int64_t t0=0,t1=0,t2=0;if(!parse_clock_sync_line(line,path.generation,t0,t1,t2)||t1==0||t2==0)return false;auto e=estimate_clock_offset(t0,t1,t2,static_cast<std::int64_t>(monotonic_us()));if(!e.valid)return true;clock_offset_us.store(e.offset_us);current_rtt_us.store(e.rtt_us);clock_valid.store(true,std::memory_order_release);return true;}
    void loop(){
        reassembler.reset(path.generation,path.session_id);
        replay.reset();
        std::array<std::uint8_t,kVideoMaxDatagramBytes+1> wire{};
        while(run.load()){
            bool authenticated_packet=false;
            sockaddr_storage source{};
            socklen_t source_len=sizeof(source);
            const int received=recv_datagram(path.socket.fd,wire,source,source_len,20);
            if(received>0&&received<=static_cast<int>(kVideoMaxDatagramBytes)){
                const std::uint64_t arrival=monotonic_us();
                const std::span<const std::uint8_t> bytes(wire.data(),static_cast<std::size_t>(received));
                VideoPacketHeader header;
                if(parse_video_header(bytes,header)&&header.generation==path.generation&&header.session_id==path.session_id&&
                   header.media_type!=VideoMediaType::Probe&&header.media_type!=VideoMediaType::ProbeAck&&
                   bytes.size()==kVideoHeaderBytes+static_cast<std::size_t>(header.payload_length)+kVideoAeadTagBytes){
                    std::size_t plaintext_size=0;
                    if(cipher&&cipher->open(header.packet_sequence,bytes.first(kVideoHeaderBytes),bytes.subspan(kVideoHeaderBytes),plaintext_buffer,plaintext_size)&&
                       plaintext_size==header.payload_length&&replay.accept(header.packet_sequence)){
                        authenticated_packet=true;
                        last_media_packet=Clock::now();
                        note_sequence(header.packet_sequence);
                        if(header.media_type!=VideoMediaType::Keepalive){
                            note_arrival(header.frame_id,arrival);
                            const std::span<const std::uint8_t> plaintext(plaintext_buffer.data(),plaintext_size);
                            const auto status=reassembler.accept(header,plaintext,assembled);
                            if(status==ReassemblyStatus::NeedIdr)request_idr();
                            else if(status==ReassemblyStatus::Complete){
                                const auto first=take_arrival(assembled.frame_id,arrival);
                                handle_complete(assembled,static_cast<double>(arrival-first)/1000.0,first);
                            }
                        }
                    }
                }
            }
            if(!authenticated_packet&&media.load()&&last_media_packet.time_since_epoch().count()!=0&&Clock::now()-last_media_packet>=std::chrono::seconds(1)){
                fail(VideoReceiverFailure::MediaStall);break;
            }
            control_tick();
        }
        audio_output.close();presenter.close();window.store(0);decoder.flush();
    }
};

VideoReceiver::VideoReceiver():impl_(std::make_unique<Impl>()){}
bool VideoReceiver::start(DirectVideoPath path,std::function<void(const std::string&)>send){stop();impl_=std::make_unique<Impl>();if(path.socket.fd<0||path.peer_len==0||path.session_id==0||path.generation==0)return false;impl_->path=std::move(path);impl_->control_send=std::move(send);impl_->cipher=std::make_unique<VideoCipher>(impl_->path.keys);if(!impl_->cipher->valid())return false;impl_->run.store(true);impl_->thread=std::thread([this]{impl_->loop();});return true;}
bool VideoReceiver::handle_control_line(const std::string&line){return impl_&&impl_->handle_control(line);}bool VideoReceiver::media_started()const{return impl_&&impl_->media.load();}bool VideoReceiver::failed()const{return impl_&&impl_->failure.load()!=VideoReceiverFailure::NoFailure;}VideoReceiverFailure VideoReceiver::failure_reason()const{return impl_?impl_->failure.load():VideoReceiverFailure::NoFailure;}Window VideoReceiver::presentation_window()const{return impl_?static_cast<Window>(impl_->window.load()):0;}std::uint64_t VideoReceiver::stale_frames()const{return impl_?impl_->stale.load():0;}std::uint64_t VideoReceiver::highest_sequence()const{return impl_?impl_->highest.load():0;}std::uint32_t VideoReceiver::audio_queued_ms()const{return impl_?impl_->audio_output.queued_ms():0;}void VideoReceiver::stop(){if(!impl_)return;impl_->run.store(false);if(impl_->thread.joinable())impl_->thread.join();}VideoReceiver::~VideoReceiver(){stop();}
}
