#include <opal/video_capture.hpp>
#include <opal/video_decoder.hpp>
#include <cassert>
#include <cstdlib>

int main(){
    setenv("OPAL_DECODER","software",1);
    setenv("OPAL_CAPTURE_CMD","ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc=size=320x180:rate=60 -frames:v 60 -pix_fmt yuv420p -c:v libx264 -preset ultrafast -tune zerolatency -bf 0 -g 1 -keyint_min 1 -sc_threshold 0 -an -f flv pipe:1",1);
    opal::VideoCapture capture;assert(capture.start({320,180,60},8000,false,""));
    opal::VideoDecoder decoder;bool configured=false;
    for(const auto &config:capture.configs())if(config.kind==opal::MediaKind::VideoH264){assert(decoder.configure_h264(config.extradata));configured=true;}
    assert(configured);assert(decoder.backend_name().rfind("software",0)==0);
    int encoded=0,decoded=0;std::size_t superseded_total=0;opal::EncodedMediaUnit unit;
    while(encoded<4){
        if(!capture.next(unit,1000))break;if(unit.kind!=opal::MediaKind::VideoH264)continue;++encoded;
        opal::DecodedVideoView latest;std::size_t superseded=0;assert(decoder.decode_latest(unit.data,unit.pts_us,latest,superseded));superseded_total+=superseded;
        if(latest.frame){
            assert(latest.frame->width==320&&latest.frame->height==180);assert(latest.pts_us==unit.pts_us);
            opal::DecodedVideoFrame owned{};assert(decoder.take_latest(owned));assert(owned.frame);assert(owned.frame->width==320&&owned.frame->height==180);assert(owned.pts_us==unit.pts_us);
            opal::DecodedVideoFrame none{};assert(!decoder.take_latest(none));av_frame_free(&owned.frame);++decoded;
        }
    }
    assert(encoded==4&&decoded==4);assert(superseded_total==0);
    decoder.flush();capture.stop();unsetenv("OPAL_CAPTURE_CMD");unsetenv("OPAL_DECODER");

    setenv("OPAL_DECODER","invalid-backend",1);
    opal::VideoDecoder invalid;
    const std::uint8_t empty=0;
    assert(!invalid.configure_h264(std::span<const std::uint8_t>(&empty,0)));
    unsetenv("OPAL_DECODER");
    return 0;
}
