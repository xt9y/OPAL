#include "capture_test_support.hpp"
#include <opal/video_capture.hpp>
#include <opal/media.hpp>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

int main(){
    if(!opal_test::capture_tests_available(true))return 0;

    const auto command=opal_test::lavfi_video_command(320,180,60,120,15,false,true);
    setenv("OPAL_CAPTURE_CMD",command.c_str(),1);
    opal::VideoCapture capture;
    assert(capture.start({320,180,60},8000,true,""));
    assert(!capture.ended());
    assert(capture.backend_name()=="external-override-flv");
    assert(capture.capture_timestamp_estimated());

    bool video=false,audio=false,keyframe=false;
    opal::EncodedMediaUnit unit;
    std::size_t observed_capacity=0;
    for(int i=0;i<300&&!(video&&audio&&keyframe);++i){
        if(!capture.next(unit,1000))continue;
        video|=unit.kind==opal::MediaKind::VideoH264&&!unit.data.empty();
        audio|=unit.kind==opal::MediaKind::AudioAac&&!unit.data.empty();
        keyframe|=unit.kind==opal::MediaKind::VideoH264&&unit.keyframe;
        observed_capacity=std::max(observed_capacity,unit.data.capacity());
    }
    assert(video&&audio&&keyframe&&observed_capacity>0);
    bool video_config=false,audio_config=false;
    for(const auto&config:capture.configs()){
        video_config|=config.kind==opal::MediaKind::VideoH264&&!config.extradata.empty();
        audio_config|=config.kind==opal::MediaKind::AudioAac&&!config.extradata.empty()&&config.sample_rate==48000&&config.channels>0;
    }
    assert(video_config&&audio_config);
    for(int i=0;i<500&&!capture.ended();++i)(void)capture.next(unit,50);
    assert(capture.ended());
    capture.stop();
    assert(!capture.ended());
    assert(capture.backend_name()=="unconfigured");
    unsetenv("OPAL_CAPTURE_CMD");

    const auto timed_command=opal_test::lavfi_video_command(320,180,60,60,15,true,false);
    setenv("OPAL_CAPTURE_CMD",timed_command.c_str(),1);
    assert(capture.start({320,180,60},8000,false,""));
    assert(capture.capture_timestamp_estimated());
    opal::EncodedMediaUnit first,second;
    for(int i=0;i<100;++i)if(capture.next(first,1000)&&first.kind==opal::MediaKind::VideoH264)break;
    assert(!first.data.empty()&&first.capture_time_us>0);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    for(int i=0;i<100;++i)if(capture.next(second,1000)&&second.kind==opal::MediaKind::VideoH264)break;
    assert(!second.data.empty()&&second.capture_time_us>first.capture_time_us);
    const auto capture_delta=second.capture_time_us-first.capture_time_us;
    assert(capture_delta<80000);
    const auto now_us=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    assert(now_us>second.capture_time_us+50000);
    capture.stop();
    unsetenv("OPAL_CAPTURE_CMD");

    auto gsr=opal::capture_command(true,60,30000,true,"",1920,1080);
    assert(gsr.find("-keyint 0.25")!=std::string::npos);
    assert(gsr.find("-tune performance")!=std::string::npos);
    assert(gsr.find("-bm cbr")!=std::string::npos);
    auto ffmpeg=opal::capture_command(false,60,30000,true,"",1920,1080);
    assert(ffmpeg.find("-bf 0")!=std::string::npos);
    assert(ffmpeg.find("-g 15")!=std::string::npos);
    return 0;
}
