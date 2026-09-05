#include "capture_test_support.hpp"
#include <opal/video_capture.hpp>
#include <opal/video_decoder.hpp>
#include <cassert>
#include <cstdlib>
#include <utility>
#include <vector>

int main(){
    if(!opal_test::capture_tests_available())return 0;
    setenv("OPAL_DECODER","software",1);
    const auto command=opal_test::lavfi_video_command(320,180,60,60,1,false,false);
    setenv("OPAL_CAPTURE_CMD",command.c_str(),1);

    opal::VideoCapture capture;
    assert(capture.start({320,180,60},8000,false,""));
    opal::VideoDecoder decoder;
    bool configured=false;
    for(const auto&config:capture.configs())if(config.kind==opal::MediaKind::VideoH264){assert(decoder.configure_h264(config.extradata));configured=true;}
    assert(configured);
    assert(decoder.backend_name().rfind("software",0)==0);

    int encoded=0,decoded=0,owned_packets=0;
    std::size_t superseded_total=0;
    opal::EncodedMediaUnit unit;
    while(encoded<6){
        if(!capture.next(unit,1000))break;
        if(unit.kind!=opal::MediaKind::VideoH264)continue;
        ++encoded;
        opal::DecodedVideoView latest;
        std::size_t superseded=0;
        if((encoded&1)==0){
            std::vector<std::uint8_t>owned_unit=unit.data;
            owned_unit.reserve(owned_unit.size()+64);
            assert(decoder.decode_latest_owned(std::move(owned_unit),unit.pts_us,latest,superseded));
            ++owned_packets;
        }else assert(decoder.decode_latest(unit.data,unit.pts_us,latest,superseded));
        superseded_total+=superseded;
        if(latest.frame){
            assert(latest.frame->width==320&&latest.frame->height==180);
            assert(latest.pts_us==unit.pts_us);
            opal::DecodedVideoFrame owned{};
            assert(decoder.take_latest(owned));
            assert(owned.frame&&owned.frame->width==320&&owned.frame->height==180&&owned.pts_us==unit.pts_us);
            opal::DecodedVideoFrame none{};
            assert(!decoder.take_latest(none));
            av_frame_free(&owned.frame);
            ++decoded;
        }
    }
    assert(encoded==6&&decoded==6&&owned_packets==3);
    assert(superseded_total==0);
    decoder.flush();capture.stop();unsetenv("OPAL_CAPTURE_CMD");unsetenv("OPAL_DECODER");

    setenv("OPAL_DECODER","invalid-backend",1);
    opal::VideoDecoder invalid;
    const std::uint8_t empty=0;
    assert(!invalid.configure_h264(std::span<const std::uint8_t>(&empty,0)));
    unsetenv("OPAL_DECODER");
    return 0;
}
