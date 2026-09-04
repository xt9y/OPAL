#include <opal/video_feedback.hpp>
#include <cassert>
#include <chrono>
#include <string>

int main(){
    using namespace std::chrono;
    const auto start=steady_clock::now();
    opal::BitrateController controller(30000);assert(controller.target_kbps()==30000);assert(controller.floor_kbps()==10500);
    opal::VideoFeedbackSample bad{};bad.received=970;bad.lost=30;bad.rtt_us=20000;
    assert(controller.on_feedback(bad,start)==22500);
    assert(controller.on_feedback(bad,start+milliseconds(100))==16875);
    opal::VideoFeedbackSample worse{};worse.received=950;worse.lost=50;worse.rtt_us=40000;
    assert(controller.on_feedback(worse,start+milliseconds(200))==12656);
    assert(controller.on_feedback(worse,start+milliseconds(300))==controller.floor_kbps());
    for(int i=0;i<20;++i)controller.on_feedback(worse,start+milliseconds(400+i*100));
    assert(controller.target_kbps()==controller.floor_kbps());

    opal::BitrateController recovery(30000);assert(recovery.on_feedback(bad,start)==22500);
    opal::VideoFeedbackSample good{};good.received=1000;good.lost=0;good.rtt_us=20000;
    recovery.on_feedback(good,start+milliseconds(100));
    assert(recovery.on_feedback(good,start+milliseconds(1101))==23625);
    for(int i=0;i<20;++i)recovery.on_feedback(good,start+milliseconds(2200+i*1100));
    assert(recovery.target_kbps()<=30000);

    auto line=opal::video_feedback_line(7,{1234,99,1,15000,3000});opal::VideoFeedbackSample parsed;
    assert(opal::parse_video_feedback_line(line,7,parsed));assert(parsed.highest_sequence==1234&&parsed.received==99&&parsed.lost==1);
    assert(!opal::parse_video_feedback_line(line,8,parsed));assert(!opal::parse_video_feedback_line(line+" extra",7,parsed));

    auto estimate=opal::estimate_clock_offset(1000000,1005500,1005600,1001100);
    assert(estimate.valid);assert(estimate.rtt_us==1000);assert(estimate.offset_us==5000);
    auto cross_boot=opal::estimate_clock_offset(200000000,1000500,1000600,200001100);
    assert(cross_boot.valid);assert(cross_boot.rtt_us==1000);assert(cross_boot.offset_us==-199000000);
    auto request=opal::clock_sync_request_line(4,123);std::int64_t t0=0,t1=0,t2=0;
    assert(opal::parse_clock_sync_line(request,4,t0,t1,t2)&&t0==123&&t1==0&&t2==0);
    auto reply=opal::clock_sync_reply_line(4,123,456,457);assert(opal::parse_clock_sync_line(reply,4,t0,t1,t2)&&t1==456&&t2==457);

    opal::HostMediaDebugSample host{};
    host.frame_id=91;host.frame_bytes=48123;host.data_fragments=44;host.fec_fragments=5;
    host.send_span_us=2800;host.capture_to_packet_us=4100;host.target_kbps=30000;host.active_kbps=28400;
    host.stale_frames=2;host.idr_requests=3;host.restarts=1;host.chain_valid=true;host.restart_reason="decode-backlog";
    const auto host_line=opal::host_media_debug_line(9,host);
    opal::HostMediaDebugSample host_parsed;
    assert(opal::parse_host_media_debug_line(host_line,9,host_parsed));
    assert(host_parsed.frame_id==91&&host_parsed.frame_bytes==48123);
    assert(host_parsed.data_fragments==44&&host_parsed.fec_fragments==5);
    assert(host_parsed.send_span_us==2800&&host_parsed.capture_to_packet_us==4100);
    assert(host_parsed.target_kbps==30000&&host_parsed.active_kbps==28400);
    assert(host_parsed.stale_frames==2&&host_parsed.idr_requests==3&&host_parsed.restarts==1&&host_parsed.chain_valid);
    assert(host_parsed.restart_reason=="decode-backlog");
    assert(!opal::parse_host_media_debug_line(host_line,10,host_parsed));
    assert(!opal::parse_host_media_debug_line(host_line+" extra",9,host_parsed));
    auto invalid_bool=host_line;const auto bool_pos=invalid_bool.rfind(" 1 ");assert(bool_pos!=std::string::npos);invalid_bool.replace(bool_pos+1,1,"2");
    assert(!opal::parse_host_media_debug_line(invalid_bool,9,host_parsed));
    const auto human=opal::format_host_media_debug(host_parsed);
    assert(human.find("frame=91")!=std::string::npos);
    assert(human.find("send=2.8ms")!=std::string::npos);
    assert(human.find("chain=ok")!=std::string::npos);
    assert(human.find("restart_reason=decode-backlog")!=std::string::npos);
    assert(human.find("key=")==std::string::npos&&human.find("token=")==std::string::npos&&human.find("password=")==std::string::npos);

    const auto debug_on=opal::debug_media_request_line(9,true);bool debug_enabled=false;
    assert(debug_on=="DEBUG_MEDIA 9 1");
    assert(opal::parse_debug_media_request_line(debug_on,9,debug_enabled)&&debug_enabled);
    const auto debug_off=opal::debug_media_request_line(9,false);
    assert(opal::parse_debug_media_request_line(debug_off,9,debug_enabled)&&!debug_enabled);
    assert(!opal::parse_debug_media_request_line(debug_on,8,debug_enabled));
    assert(!opal::parse_debug_media_request_line(debug_on+" extra",9,debug_enabled));
    assert(!opal::parse_debug_media_request_line("DEBUG_MEDIA 9 2",9,debug_enabled));

    opal::LatencyTelemetry latency{};
    latency.capture_to_packet_ms=4.1;latency.network_ms=23.5;latency.reassembly_ms=3.2;
    latency.decode_ms=4.7;latency.present_ms=2.1;latency.total_ms=37.6;latency.loss_percent=0.2;
    latency.stale_frames=2;latency.bitrate_kbps=28400;latency.decoder_backend="software-slice";
    latency.decoded_fps=59.4;latency.presented_fps=59.1;latency.video_queue_depth=3;latency.skipped_present_frames=7;
    const auto latency_line=opal::format_latency_telemetry(latency);
    assert(latency_line.find("decoder=software-slice")!=std::string::npos);
    assert(latency_line.find("decode_fps=59.4")!=std::string::npos);
    assert(latency_line.find("present_fps=59.1")!=std::string::npos);
    assert(latency_line.find("queue=3")!=std::string::npos);
    assert(latency_line.find("skip_present=7")!=std::string::npos);
    return 0;
}
