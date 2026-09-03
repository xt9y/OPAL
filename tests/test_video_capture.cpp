#include <opal/video_capture.hpp>
#include <opal/media.hpp>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

int main(){
    const char *command=
        "ffmpeg -hide_banner -loglevel error "
        "-f lavfi -i testsrc=size=320x180:rate=60 "
        "-f lavfi -i sine=frequency=440:sample_rate=48000 "
        "-t 2 -pix_fmt yuv420p "
        "-c:v libx264 -preset ultrafast -tune zerolatency -bf 0 "
        "-g 15 -keyint_min 15 -sc_threshold 0 "
        "-c:a aac -b:a 96k -f flv pipe:1";
    setenv("OPAL_CAPTURE_CMD",command,1);

    opal::VideoCapture capture;
    assert(capture.start({320,180,60},8000,true,""));
    assert(!capture.ended());
    bool video=false,audio=false,keyframe=false;
    for(int i=0;i<300&&!(video&&audio&&keyframe);++i){
        opal::EncodedMediaUnit unit;
        if(!capture.next(unit,1000))continue;
        video|=unit.kind==opal::MediaKind::VideoH264&&!unit.data.empty();
        audio|=unit.kind==opal::MediaKind::AudioAac&&!unit.data.empty();
        keyframe|=unit.kind==opal::MediaKind::VideoH264&&unit.keyframe;
    }
    assert(video&&audio&&keyframe);
    bool video_config=false,audio_config=false;
    for(const auto &config:capture.configs()){
        video_config|=config.kind==opal::MediaKind::VideoH264&&!config.extradata.empty();
        audio_config|=config.kind==opal::MediaKind::AudioAac&&!config.extradata.empty()&&config.sample_rate==48000&&config.channels>0;
    }
    assert(video_config&&audio_config);

    // Drain the bounded fixture. A temporary read timeout may still return
    // false while the process is alive, but permanent EOF must become visible
    // so VideoSender can restart capture instead of spinning forever.
    for(int i=0;i<500&&!capture.ended();++i){
        opal::EncodedMediaUnit unit;
        (void)capture.next(unit,50);
    }
    assert(capture.ended());
    capture.stop();
    assert(!capture.ended());
    unsetenv("OPAL_CAPTURE_CMD");

    // capture_time_us must follow the encoded capture timeline rather than the
    // moment the application happened to drain libavformat. Deliberately stop
    // reading for 150 ms: the next 60 Hz video packet should still be only a
    // few frame intervals newer, exposing the reader backlog through its age.
    const char *timed_command=
        "ffmpeg -hide_banner -loglevel error -re "
        "-f lavfi -i testsrc=size=320x180:rate=60 -t 1 -pix_fmt yuv420p "
        "-c:v libx264 -preset ultrafast -tune zerolatency -bf 0 "
        "-g 15 -keyint_min 15 -sc_threshold 0 -an -f flv pipe:1";
    setenv("OPAL_CAPTURE_CMD",timed_command,1);
    assert(capture.start({320,180,60},8000,false,""));
    opal::EncodedMediaUnit first,second;
    for(int i=0;i<100;++i){
        if(capture.next(first,1000)&&first.kind==opal::MediaKind::VideoH264)break;
    }
    assert(!first.data.empty()&&first.capture_time_us>0);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    for(int i=0;i<100;++i){
        if(capture.next(second,1000)&&second.kind==opal::MediaKind::VideoH264)break;
    }
    assert(!second.data.empty()&&second.capture_time_us>first.capture_time_us);
    const auto capture_delta=second.capture_time_us-first.capture_time_us;
    assert(capture_delta<80000);
    const auto now_us=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    assert(now_us>second.capture_time_us+50000);
    capture.stop();
    unsetenv("OPAL_CAPTURE_CMD");

    auto gsr=opal::capture_command(true,60,30000,true,"",1920,1080);
    assert(gsr.find("-keyint 0.25")!=std::string::npos);
    assert(gsr.find("-keyint 1")==std::string::npos);
    assert(gsr.find("-tune performance")!=std::string::npos);
    assert(gsr.find("-bm cbr")!=std::string::npos);

    auto ffmpeg=opal::capture_command(false,60,30000,true,"",1920,1080);
    assert(ffmpeg.find("-bf 0")!=std::string::npos);
    assert(ffmpeg.find("-g 15")!=std::string::npos);
    assert(ffmpeg.find("-keyint_min 15")!=std::string::npos);
    return 0;
}
