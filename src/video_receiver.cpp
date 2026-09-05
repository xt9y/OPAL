#include <opal/video_receiver.hpp>

#include <opal/audio_output.hpp>
#include <opal/latency_window.hpp>
#include <opal/udp_transport.hpp>
#include <opal/video_backlog.hpp>
#include <opal/video_decoder.hpp>
#include <opal/video_feedback.hpp>
#include <opal/video_packet.hpp>
#include <opal/video_reassembly.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>

namespace opal { namespace {
using Clock=std::chrono::steady_clock;
constexpr std::uint64_t kMediaStallRecoveryUs=500000;
constexpr std::uint64_t kMediaStallFailureUs=3000000;
constexpr std::size_t kVideoDecodeBurstFrames=6;
std::uint64_t monotonic_us(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());}
bool debug_enabled(){const char *v=std::getenv("OPAL_DEBUG");return v&&*v&&std::string(v)!="0";}
std::uint32_t read32(std::span<const std::uint8_t>b){return b.size()<4?0:(static_cast<std::uint32_t>(b[0])<<24)|(static_cast<std::uint32_t>(b[1])<<16)|(static_cast<std::uint32_t>(b[2])<<8)|b[3];}
double ewma(double old,double sample){return old==0.0?sample:old*0.8+sample*0.2;}
}

struct VideoReceiver::Impl{
    struct Arrival{std::uint64_t id=0,us=0;};
    struct MediaItem{ReassembledFrame frame;double reassembly_ms=0.0;std::uint64_t first_arrival_us=0;};

    std::unique_ptr<DirectVideoPath> owned_path;
    VideoKeys keys;
    std::uint64_t session_id=0;
    std::uint32_t generation=0;
    bool native_mode=false;
    std::function<void(const std::string&)>control_send;
    VideoReassembler reassembler;
    ReplayWindow1024 replay;
    std::unique_ptr<VideoCipher>cipher;
    std::array<std::uint8_t,kVideoPlaintextBytes>plaintext_buffer{};
    VideoDecoder decoder;
    AudioOutput audio_output;
    ReassembledFrame assembled;
    std::array<Arrival,8>arrivals{};
    std::size_t arrival_cursor=0;

    std::thread rx_thread,media_thread,control_thread;
    mutable std::mutex media_mu,idr_mu,telemetry_mu,rx_mu,sequence_mu,frame_mu;
    std::condition_variable media_cv;
    VideoBacklog<MediaItem,kVideoDecodeBurstFrames>video_frames;
    std::optional<MediaItem>video_config,audio_frame,audio_config;
    DecodedVideoFrame latest_frame{};

    std::atomic<bool>run{false},media{false},clock_valid{false},force_reassembly_idr{false},audio_reset_requested{false},stall_reset_requested{false};
    std::atomic<VideoReceiverFailure>failure{VideoReceiverFailure::NoFailure};
    std::atomic<std::uint64_t>stale{0},highest{0},kernel_drops{0},decoded_frames{0},presented_frames{0},skipped_present_frames{0},encoded_drops{0},last_media_us{0},last_video_us{0},last_stall_recovery_us{0};
    std::atomic<std::int64_t>latest_video_ts{0},clock_offset_us{0};
    std::atomic<std::uint32_t>current_rtt_us{0},last_decode_age_us{0},host_capture_to_packet_us{0},audio_queued_debug{0},video_backlog_debug{0},stall_recoveries{0};
    std::atomic<int>host_active_kbps{0};
    bool decoder_ready=false,audio_ready=false;
    Clock::time_point last_idr_request{},last_feedback{},last_clock{},last_debug{};
    std::uint64_t interval_first=0,interval_highest=0,feedback_cursor=0,last_debug_decoded=0,last_debug_presented=0;
    std::uint32_t interval_received=0;
    LatencyTelemetry telemetry;
    LatencyWindow<128>network_latency,reassembly_latency,decode_latency,present_latency,total_latency;

    bool initialize(VideoKeys video_keys,std::uint64_t sid,std::uint32_t gen,std::function<void(const std::string&)>send){
        if(sid==0||gen==0)return false;
        keys=video_keys;session_id=sid;generation=gen;control_send=std::move(send);
        cipher=std::make_unique<VideoCipher>(keys);
        if(!cipher->valid())return false;
        reassembler.reset(generation,session_id);replay.reset();return true;
    }

    void fail(VideoReceiverFailure reason){auto expected=VideoReceiverFailure::NoFailure;failure.compare_exchange_strong(expected,reason);run.store(false);media_cv.notify_all();}
    void note_arrival(std::uint64_t id,std::uint64_t us){for(const auto&s:arrivals)if(s.id==id)return;for(auto&s:arrivals)if(s.id==0){s={id,us};return;}arrivals[arrival_cursor%arrivals.size()]={id,us};arrival_cursor=(arrival_cursor+1)%arrivals.size();}
    std::uint64_t take_arrival(std::uint64_t id,std::uint64_t fallback){for(auto&s:arrivals)if(s.id==id){const auto us=s.us;s={};return us;}return fallback;}

    void sync_video_backlog_debug(){video_backlog_debug.store(static_cast<std::uint32_t>(video_frames.size()),std::memory_order_release);}
    void clear_video_backlog(){video_frames.clear();sync_video_backlog_debug();}
    VideoBacklogPush push_video_backlog(MediaItem item,bool keyframe){const auto result=video_frames.push(std::move(item),keyframe);sync_video_backlog_debug();return result;}
    std::optional<MediaItem>pop_video_backlog(){auto item=video_frames.pop();sync_video_backlog_debug();return item;}

    void clear_latest_frame(){std::lock_guard<std::mutex>lock(frame_mu);if(latest_frame.frame)av_frame_free(&latest_frame.frame);latest_frame.pts_us=0;}
    void publish_latest(DecodedVideoFrame owned){
        if(!owned.frame)return;
        std::lock_guard<std::mutex>lock(frame_mu);
        if(latest_frame.frame){av_frame_free(&latest_frame.frame);stale.fetch_add(1);skipped_present_frames.fetch_add(1);}
        latest_frame=owned;
        media.store(true,std::memory_order_release);
    }
    bool take_latest(DecodedVideoFrame&out){std::lock_guard<std::mutex>lock(frame_mu);if(!latest_frame.frame)return false;if(out.frame)av_frame_free(&out.frame);out=latest_frame;latest_frame={};return true;}

    void request_idr_control(const char*reason){const auto now=Clock::now();{std::lock_guard<std::mutex>lock(idr_mu);if(last_idr_request.time_since_epoch().count()!=0&&now-last_idr_request<std::chrono::milliseconds(250))return;last_idr_request=now;}stale.fetch_add(1);audio_reset_requested.store(true);media_cv.notify_one();if(control_send)control_send("REQUEST_IDR "+std::to_string(generation)+" "+(reason&&*reason?reason:"unknown"));}
    void request_idr_rx(const char*reason="reassembly-loss"){reassembler.require_idr();request_idr_control(reason);}
    void request_idr_media(const char*reason="decode-failure"){force_reassembly_idr.store(true);request_idr_control(reason);}

    void update_decode_telemetry(const ReassembledFrame&a,double reassembly_ms,double decode_ms,std::uint64_t first,std::uint64_t done){
        std::lock_guard<std::mutex>lock(telemetry_mu);
        reassembly_latency.push(reassembly_ms);decode_latency.push(decode_ms);
        telemetry.reassembly_ms=ewma(telemetry.reassembly_ms,reassembly_ms);telemetry.decode_ms=ewma(telemetry.decode_ms,decode_ms);telemetry.stale_frames=stale.load();telemetry.video_queue_depth=video_backlog_debug.load();telemetry.skipped_present_frames=skipped_present_frames.load();telemetry.decoder_backend=decoder_ready?decoder.backend_name():"unconfigured";
        if(clock_valid.load()){
            const auto offset=clock_offset_us.load();
            const double network=std::max(0.0,static_cast<double>(static_cast<std::int64_t>(first)-static_cast<std::int64_t>(a.capture_timestamp_us)+offset)/1000.0);
            const double decode_age=std::max(0.0,static_cast<double>(static_cast<std::int64_t>(done)-static_cast<std::int64_t>(a.capture_timestamp_us)+offset)/1000.0);
            network_latency.push(network);telemetry.network_ms=ewma(telemetry.network_ms,network);last_decode_age_us.store(static_cast<std::uint32_t>(std::min(4294967295.0,decode_age*1000.0)));
        }else last_decode_age_us.store(0);
    }
    void update_present_telemetry(std::int64_t pts_us,double present_ms,std::uint64_t done){
        std::lock_guard<std::mutex>lock(telemetry_mu);
        present_latency.push(present_ms);telemetry.present_ms=ewma(telemetry.present_ms,present_ms);telemetry.stale_frames=stale.load();telemetry.video_queue_depth=video_backlog_debug.load();telemetry.skipped_present_frames=skipped_present_frames.load();
        if(clock_valid.load()){
            const auto offset=clock_offset_us.load();
            const double total=std::max(0.0,static_cast<double>(static_cast<std::int64_t>(done)-pts_us+offset)/1000.0);
            total_latency.push(total);telemetry.total_ms=ewma(telemetry.total_ms,total);
        }
    }

    bool decode_and_publish(MediaItem&item){
        auto&a=item.frame;const auto decode_begin=monotonic_us();
        if(const char*stall=std::getenv("OPAL_TEST_DECODE_STALL_MS");stall&&*stall)try{const int ms=std::clamp(std::stoi(stall),0,2000);if(ms)std::this_thread::sleep_for(std::chrono::milliseconds(ms));}catch(...){ }
        DecodedVideoView newest;std::size_t superseded=0;
        if(!decoder_ready||!decoder.decode_latest_owned(std::move(a.data),static_cast<std::int64_t>(a.capture_timestamp_us),newest,superseded)){decoder.flush();request_idr_media("decode-failure");return false;}
        const auto decode_end=monotonic_us();bool skip_publish=false;
        if(newest.frame){decoded_frames.fetch_add(static_cast<std::uint64_t>(superseded)+1);if(superseded)stale.fetch_add(superseded);{std::lock_guard<std::mutex>lock(media_mu);skip_publish=!video_frames.empty();}if(skip_publish){skipped_present_frames.fetch_add(1);stale.fetch_add(1);}}
        update_decode_telemetry(a,item.reassembly_ms,static_cast<double>(decode_end-decode_begin)/1000.0,item.first_arrival_us,decode_end);
        if(newest.frame&&!skip_publish){DecodedVideoFrame owned{};if(decoder.take_latest(owned))publish_latest(owned);}
        return true;
    }

    void handle_media_complete(MediaItem&item){
        auto&a=item.frame;
        if(a.media_type==VideoMediaType::VideoH264){
            if(a.config){decoder.flush();decoder_ready=decoder.configure_h264(a.data);{std::lock_guard<std::mutex>lock(telemetry_mu);telemetry.decoder_backend=decoder_ready?decoder.backend_name():"unconfigured";}if(!decoder_ready)request_idr_media("decode-failure");return;}
            decode_and_publish(item);return;
        }
        if(a.media_type==VideoMediaType::AudioAac){
            if(a.config){if(a.data.size()<5){audio_ready=false;return;}const int rate=static_cast<int>(read32(a.data)),channels=a.data[4];audio_ready=audio_output.configure_aac(std::span<const std::uint8_t>(a.data).subspan(5),rate,channels);audio_queued_debug.store(audio_output.queued_ms());return;}
            if(audio_ready){audio_output.submit(a.data,static_cast<std::int64_t>(a.capture_timestamp_us),latest_video_ts.load());audio_queued_debug.store(audio_output.queued_ms());}
        }
    }

    void enqueue_media(MediaItem item){
        bool dropped=false;
        {std::lock_guard<std::mutex>lock(media_mu);
            if(item.frame.media_type==VideoMediaType::VideoH264){
                if(item.frame.config){clear_video_backlog();video_config=std::move(item);}
                else{
                    const bool keyframe=item.frame.keyframe;
                    const auto result=push_video_backlog(std::move(item),keyframe);
                    if(result==VideoBacklogPush::DroppedIncoming)dropped=true;
                }
            }else if(item.frame.media_type==VideoMediaType::AudioAac){if(item.frame.config)audio_config=std::move(item);else audio_frame=std::move(item);}
        }
        if(dropped){encoded_drops.fetch_add(1);stale.fetch_add(1);skipped_present_frames.fetch_add(1);}
        media_cv.notify_one();
    }

    void media_loop(){
        while(run.load()){
            if(stall_reset_requested.exchange(false)){decoder.flush();clear_latest_frame();latest_video_ts.store(0);}
            if(audio_reset_requested.exchange(false)){audio_output.reset_to(latest_video_ts.load());audio_queued_debug.store(audio_output.queued_ms());}
            std::optional<MediaItem>item;
            {std::unique_lock<std::mutex>lock(media_mu);media_cv.wait_for(lock,std::chrono::milliseconds(20),[&]{return !run.load()||video_config||!video_frames.empty()||audio_config||audio_frame||audio_reset_requested.load()||stall_reset_requested.load();});if(!run.load())break;if(video_config){item=std::move(video_config);video_config.reset();}else if(!video_frames.empty())item=pop_video_backlog();else if(audio_config){item=std::move(audio_config);audio_config.reset();}else if(audio_frame){item=std::move(audio_frame);audio_frame.reset();}}
            if(item)handle_media_complete(*item);
        }
        audio_output.close();clear_latest_frame();decoder.flush();
    }

    void note_sequence(std::uint64_t seq){auto prev=highest.load();while(seq>prev&&!highest.compare_exchange_weak(prev,seq)){}std::lock_guard<std::mutex>lock(sequence_mu);if(feedback_cursor&&seq<feedback_cursor)return;if(interval_received==0){interval_first=feedback_cursor?feedback_cursor:seq;interval_highest=seq;interval_received=1;}else{interval_highest=std::max(interval_highest,seq);++interval_received;}}

    bool accept_wire(std::span<const std::uint8_t>bytes){
        if(!run.load()||bytes.empty()||bytes.size()>kVideoMaxDatagramBytes)return false;
        std::lock_guard<std::mutex>rx_lock(rx_mu);
        if(force_reassembly_idr.exchange(false))reassembler.require_idr();
        const std::uint64_t arrival=monotonic_us();VideoPacketHeader header;
        if(!parse_video_header(bytes,header)||header.generation!=generation||header.session_id!=session_id||header.media_type==VideoMediaType::Probe||header.media_type==VideoMediaType::ProbeAck||bytes.size()!=kVideoHeaderBytes+static_cast<std::size_t>(header.payload_length)+kVideoAeadTagBytes)return false;
        std::size_t plaintext_size=0;
        if(!cipher||!cipher->open(header.packet_sequence,bytes.first(kVideoHeaderBytes),bytes.subspan(kVideoHeaderBytes),plaintext_buffer,plaintext_size)||plaintext_size!=header.payload_length||!replay.accept(header.packet_sequence))return false;
        last_media_us.store(arrival);note_sequence(header.packet_sequence);if(header.media_type==VideoMediaType::Keepalive)return true;
        if(header.media_type==VideoMediaType::VideoH264||header.media_type==VideoMediaType::Fec)last_video_us.store(arrival);
        note_arrival(header.frame_id,arrival);
        const std::span<const std::uint8_t>plaintext(plaintext_buffer.data(),plaintext_size);const auto status=reassembler.accept(header,plaintext,assembled);
        if(status==ReassemblyStatus::NeedIdr)request_idr_rx("reassembly-loss");
        else if(status==ReassemblyStatus::Complete){if(assembled.media_type==VideoMediaType::VideoH264){stall_recoveries.store(0);last_stall_recovery_us.store(0);}const auto first=take_arrival(assembled.frame_id,arrival);MediaItem item{std::move(assembled),static_cast<double>(arrival-first)/1000.0,first};assembled={};enqueue_media(std::move(item));}
        return true;
    }

    void control_tick(){
        const auto now=Clock::now();
        if(last_feedback.time_since_epoch().count()==0||now-last_feedback>=std::chrono::milliseconds(100)){
            VideoFeedbackSample s;s.highest_sequence=highest.load();
            {std::lock_guard<std::mutex>lock(sequence_mu);s.received=interval_received;if(interval_received&&interval_first&&interval_highest>=interval_first){const auto expected=interval_highest-interval_first+1;s.lost=static_cast<std::uint32_t>(std::min<std::uint64_t>(0xffffffffULL,expected>interval_received?expected-interval_received:0));feedback_cursor=interval_highest+1;}interval_first=interval_highest=0;interval_received=0;}
            s.rtt_us=current_rtt_us.load();s.decode_age_us=last_decode_age_us.load();if(control_send)control_send(video_feedback_line(generation,s));const auto total=static_cast<std::uint64_t>(s.received)+s.lost;
            {std::lock_guard<std::mutex>lock(telemetry_mu);telemetry.capture_to_packet_ms=static_cast<double>(host_capture_to_packet_us.load())/1000.0;telemetry.bitrate_kbps=host_active_kbps.load();telemetry.loss_percent=total?100.0*s.lost/static_cast<double>(total):0.0;telemetry.video_queue_depth=video_backlog_debug.load();telemetry.skipped_present_frames=skipped_present_frames.load();}
            last_feedback=now;
        }
        if(last_clock.time_since_epoch().count()==0||now-last_clock>=std::chrono::seconds(1)){if(control_send)control_send(clock_sync_request_line(generation,static_cast<std::int64_t>(monotonic_us())));last_clock=now;}
        if(debug_enabled()&&(last_debug.time_since_epoch().count()==0||now-last_debug>=std::chrono::seconds(1))){
            const auto decoded_now=decoded_frames.load(),presented_now=presented_frames.load();const double elapsed=last_debug.time_since_epoch().count()==0?0.0:std::chrono::duration<double>(now-last_debug).count();LatencyTelemetry snapshot;LatencyPercentiles net_tail,reassembly_tail,decode_tail,present_tail,total_tail;
            {std::lock_guard<std::mutex>lock(telemetry_mu);telemetry.stale_frames=stale.load();telemetry.video_queue_depth=video_backlog_debug.load();telemetry.skipped_present_frames=skipped_present_frames.load();if(elapsed>0.0){telemetry.decoded_fps=static_cast<double>(decoded_now-last_debug_decoded)/elapsed;telemetry.presented_fps=static_cast<double>(presented_now-last_debug_presented)/elapsed;}snapshot=telemetry;net_tail=network_latency.snapshot();reassembly_tail=reassembly_latency.snapshot();decode_tail=decode_latency.snapshot();present_tail=present_latency.snapshot();total_tail=total_latency.snapshot();}
            std::cerr<<format_latency_telemetry(snapshot)
                <<" tail_ms[p50/p95/p99] net="<<net_tail.p50<<"/"<<net_tail.p95<<"/"<<net_tail.p99
                <<" reassembly="<<reassembly_tail.p50<<"/"<<reassembly_tail.p95<<"/"<<reassembly_tail.p99
                <<" decode="<<decode_tail.p50<<"/"<<decode_tail.p95<<"/"<<decode_tail.p99
                <<" present="<<present_tail.p50<<"/"<<present_tail.p95<<"/"<<present_tail.p99
                <<" media_age="<<total_tail.p50<<"/"<<total_tail.p95<<"/"<<total_tail.p99
                <<" audio="<<audio_queued_debug.load()<<"ms rtt="<<static_cast<double>(current_rtt_us.load())/1000.0<<"ms kernel_drop="<<kernel_drops.load()<<" encoded_drop="<<encoded_drops.load()<<" stall_recovery="<<stall_recoveries.load()<<"\n";
            last_debug_decoded=decoded_now;last_debug_presented=presented_now;last_debug=now;
        }
    }

    bool handle_control(const std::string&line){
        HostMediaDebugSample host_debug;
        if(parse_host_media_debug_line(line,generation,host_debug)){host_capture_to_packet_us.store(host_debug.capture_to_packet_us);host_active_kbps.store(host_debug.active_kbps);if(debug_enabled())std::cerr<<format_host_media_debug(host_debug)<<"\n";return true;}
        std::int64_t t0=0,t1=0,t2=0;if(!parse_clock_sync_line(line,generation,t0,t1,t2)||t1==0||t2==0)return false;auto e=estimate_clock_offset(t0,t1,t2,static_cast<std::int64_t>(monotonic_us()));if(!e.valid)return true;clock_offset_us.store(e.offset_us);current_rtt_us.store(e.rtt_us);clock_valid.store(true);return true;
    }

    bool recover_stall(){
        const auto last=last_video_us.load();const auto now=monotonic_us();
        if(!media.load()||!last||now<=last+kMediaStallRecoveryUs)return true;
        const auto last_media=last_media_us.load();
        if(now>=last+kMediaStallFailureUs&&(!last_media||now>=last_media+kMediaStallFailureUs)){fail(VideoReceiverFailure::MediaStall);return false;}
        const auto previous_recovery=last_stall_recovery_us.load();
        if(previous_recovery&&now<previous_recovery+kMediaStallRecoveryUs)return true;
        const auto attempt=stall_recoveries.fetch_add(1)+1;
        {
            std::lock_guard<std::mutex>lock(rx_mu);
            reassembler.require_idr();assembled={};arrivals.fill({});arrival_cursor=0;
        }
        {
            std::lock_guard<std::mutex>lock(media_mu);
            clear_video_backlog();audio_frame.reset();
        }
        stall_reset_requested.store(true);audio_reset_requested.store(true);last_stall_recovery_us.store(now);media_cv.notify_one();
        if(attempt==1)request_idr_control("reassembly-loss");
        const double media_silence_ms=last_media&&now>last_media?static_cast<double>(now-last_media)/1000.0:0.0;
        if(debug_enabled())std::cerr<<"OPAL media stall local-recovery attempt="<<attempt<<" video_silence="<<static_cast<double>(now-last)/1000.0<<"ms media_silence="<<media_silence_ms<<"ms hard_fail="<<static_cast<double>(kMediaStallFailureUs)/1000.0<<"ms\n";
        return true;
    }

    void direct_loop(){std::array<std::array<std::uint8_t,kVideoMaxDatagramBytes+1>,kUdpReceiveBatchMax>wire{};std::array<UdpReceiveSlot,kUdpReceiveBatchMax>slots{};for(std::size_t i=0;i<slots.size();++i)slots[i].buffer=wire[i];while(run.load()){const int batch=recv_datagrams_batch(owned_path->socket.fd,slots,20);if(batch>0)for(int index=0;index<batch&&run.load();++index){auto&slot=slots[static_cast<std::size_t>(index)];kernel_drops.store(std::max<std::uint64_t>(kernel_drops.load(),slot.kernel_drops));if(slot.size)accept_wire(std::span<const std::uint8_t>(slot.buffer.data(),slot.size));}control_tick();if(!recover_stall())break;}media_cv.notify_all();}
    void native_control_loop(){while(run.load()){control_tick();if(!recover_stall())break;std::this_thread::sleep_for(std::chrono::milliseconds(20));}media_cv.notify_all();}
};

VideoReceiver::VideoReceiver():impl_(std::make_unique<Impl>()){}
bool VideoReceiver::start(DirectVideoPath path,std::function<void(const std::string&)>send){stop();impl_=std::make_unique<Impl>();if(path.socket.fd<0||path.peer_len==0||path.session_id==0||path.generation==0)return false;impl_->owned_path=std::make_unique<DirectVideoPath>(std::move(path));auto*owned=impl_->owned_path.get();if(!impl_->initialize(owned->keys,owned->session_id,owned->generation,std::move(send)))return false;impl_->run.store(true);impl_->media_thread=std::thread([this]{impl_->media_loop();});impl_->rx_thread=std::thread([this]{impl_->direct_loop();});return true;}
bool VideoReceiver::start_native(const VideoKeys&keys,std::uint64_t session_id,std::uint32_t generation,std::function<void(const std::string&)>send){stop();impl_=std::make_unique<Impl>();if(!impl_->initialize(keys,session_id,generation,std::move(send)))return false;impl_->native_mode=true;impl_->run.store(true);impl_->media_thread=std::thread([this]{impl_->media_loop();});impl_->control_thread=std::thread([this]{impl_->native_control_loop();});return true;}
bool VideoReceiver::accept_datagram(std::span<const std::uint8_t>wire){return impl_&&impl_->native_mode&&impl_->accept_wire(wire);}
bool VideoReceiver::handle_control_line(const std::string&line){return impl_&&impl_->handle_control(line);}
bool VideoReceiver::media_started()const{return impl_&&impl_->media.load();}
bool VideoReceiver::failed()const{return impl_&&impl_->failure.load()!=VideoReceiverFailure::NoFailure;}
VideoReceiverFailure VideoReceiver::failure_reason()const{return impl_?impl_->failure.load():VideoReceiverFailure::NoFailure;}
bool VideoReceiver::take_latest_video(DecodedVideoFrame&out){return impl_&&impl_->take_latest(out);}
void VideoReceiver::note_presented_video(std::int64_t pts_us,double present_ms){if(!impl_)return;impl_->presented_frames.fetch_add(1);impl_->latest_video_ts.store(pts_us);impl_->update_present_telemetry(pts_us,present_ms,monotonic_us());}
std::uint64_t VideoReceiver::stale_frames()const{return impl_?impl_->stale.load():0;}
std::uint64_t VideoReceiver::highest_sequence()const{return impl_?impl_->highest.load():0;}
std::uint64_t VideoReceiver::encoded_backlog_drops()const{return impl_?impl_->encoded_drops.load():0;}
std::uint32_t VideoReceiver::audio_queued_ms()const{return impl_?impl_->audio_queued_debug.load():0;}
void VideoReceiver::stop(){if(!impl_)return;impl_->run.store(false);impl_->media_cv.notify_all();if(impl_->rx_thread.joinable())impl_->rx_thread.join();if(impl_->control_thread.joinable())impl_->control_thread.join();impl_->media_cv.notify_all();if(impl_->media_thread.joinable())impl_->media_thread.join();impl_->clear_latest_frame();}
VideoReceiver::~VideoReceiver(){stop();}

}
