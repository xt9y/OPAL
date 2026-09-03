#include <opal/video_feedback.hpp>
#include <cassert>
#include <chrono>

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
    // steady_clock epochs are machine-local. A host that booted much later can be
    // hundreds of seconds behind the client while the measured RTT is still tiny.
    auto cross_boot=opal::estimate_clock_offset(200000000,1000500,1000600,200001100);
    assert(cross_boot.valid);assert(cross_boot.rtt_us==1000);assert(cross_boot.offset_us==-198999500);
    auto request=opal::clock_sync_request_line(4,123);std::int64_t t0=0,t1=0,t2=0;
    assert(opal::parse_clock_sync_line(request,4,t0,t1,t2)&&t0==123&&t1==0&&t2==0);
    auto reply=opal::clock_sync_reply_line(4,123,456,457);assert(opal::parse_clock_sync_line(reply,4,t0,t1,t2)&&t1==456&&t2==457);
    return 0;
}
