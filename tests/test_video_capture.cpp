#include <opal/video_capture.hpp>
#include <opal/media.hpp>
#include <cassert>
#include <cstdlib>
#include <string>

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
