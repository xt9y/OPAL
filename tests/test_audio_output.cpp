#include "capture_test_support.hpp"
#include <opal/audio_output.hpp>
#include <opal/video_capture.hpp>
#include <cassert>
#include <cstdlib>

int main(){
    if(!opal_test::capture_tests_available(true))return 0;
    const auto command=opal_test::lavfi_video_command(320,180,30,30,8,false,true);
    setenv("OPAL_CAPTURE_CMD",command.c_str(),1);
    opal::VideoCapture capture;
    assert(capture.start({320,180,30},4000,true,""));

    opal::EncodedMediaUnit unit,last_audio;
    const opal::MediaConfig*aac_config=nullptr;
    for(int attempt=0;attempt<20&&!aac_config;++attempt){
        (void)capture.next(unit,500);
        for(const auto&config:capture.configs())if(config.kind==opal::MediaKind::AudioAac){aac_config=&config;break;}
        if(capture.ended())break;
    }
    assert(aac_config&&aac_config->sample_rate==48000&&aac_config->channels>0);
    assert(capture.config_revision()>0);

    setenv("OPAL_AUDIO_TEST_SINK","hold",1);
    opal::AudioOutput output;
    assert(output.configure_aac(aac_config->extradata,aac_config->sample_rate,aac_config->channels));
    assert(output.backend_name()=="sdl3");
    int submitted=0;
    for(int i=0;i<100&&submitted<4;++i){
        if(!capture.next(unit,1000))continue;
        if(unit.kind!=opal::MediaKind::AudioAac)continue;
        last_audio=unit;
        assert(output.submit(unit.data,unit.pts_us,0));
        ++submitted;
        assert(output.queued_ms()<=24);
    }
    assert(submitted>=2);
    const auto before=output.queued_ms();
    assert(output.submit(last_audio.data,0,1000000));
    assert(output.queued_ms()==before);
    output.reset_to(1000000);
    assert(output.queued_ms()==0);
    output.close();capture.stop();
    unsetenv("OPAL_AUDIO_TEST_SINK");unsetenv("OPAL_CAPTURE_CMD");
    return 0;
}
