#include <opal/audio_output.hpp>
#include <opal/video_capture.hpp>
#include <cassert>
#include <cstdlib>

int main(){
    setenv("OPAL_CAPTURE_CMD","ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc=size=160x90:rate=30 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 1 -pix_fmt yuv420p -c:v libx264 -preset ultrafast -tune zerolatency -bf 0 -g 8 -c:a aac -b:a 96k -f flv pipe:1",1);
    opal::VideoCapture capture;assert(capture.start({160,90,30},4000,true,""));
    const opal::MediaConfig *aac_config=nullptr;
    for(const auto &config:capture.configs())if(config.kind==opal::MediaKind::AudioAac)aac_config=&config;
    assert(aac_config&&aac_config->sample_rate==48000&&aac_config->channels>0);
    setenv("OPAL_AUDIO_TEST_SINK","hold",1);
    opal::AudioOutput output;assert(output.configure_aac(aac_config->extradata,aac_config->sample_rate,aac_config->channels));
    opal::EncodedMediaUnit last_audio;int submitted=0;
    for(int i=0;i<100&&submitted<4;++i){opal::EncodedMediaUnit unit;if(!capture.next(unit,1000))continue;if(unit.kind!=opal::MediaKind::AudioAac)continue;last_audio=unit;assert(output.submit(unit.data,unit.pts_us,0));++submitted;assert(output.queued_ms()<=40);}
    assert(submitted>=2);const auto before=output.queued_ms();
    assert(output.submit(last_audio.data,0,1000000));assert(output.queued_ms()==before);
    output.reset_to(1000000);assert(output.queued_ms()==0);
    output.close();capture.stop();unsetenv("OPAL_AUDIO_TEST_SINK");unsetenv("OPAL_CAPTURE_CMD");return 0;
}
