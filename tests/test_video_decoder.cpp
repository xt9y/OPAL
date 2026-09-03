#include <opal/video_capture.hpp>
#include <opal/video_decoder.hpp>
#include <cassert>
#include <cstdlib>

int main(){
    setenv("OPAL_CAPTURE_CMD","ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc=size=320x180:rate=60 -frames:v 4 -pix_fmt yuv420p -c:v libx264 -preset ultrafast -tune zerolatency -bf 0 -g 15 -keyint_min 15 -sc_threshold 0 -an -f flv pipe:1",1);
    opal::VideoCapture capture;assert(capture.start({320,180,60},8000,false,""));
    opal::VideoDecoder decoder;bool configured=false;
    for(const auto &config:capture.configs())if(config.kind==opal::MediaKind::VideoH264){assert(decoder.configure_h264(config.extradata));configured=true;}
    assert(configured);
    int encoded=0,decoded=0;
    while(encoded<4){opal::EncodedMediaUnit unit;if(!capture.next(unit,1000))break;if(unit.kind!=opal::MediaKind::VideoH264)continue;++encoded;std::vector<opal::DecodedVideoFrame> frames;assert(decoder.decode(unit.data,unit.pts_us,frames));assert(!frames.empty());decoded+=static_cast<int>(frames.size());for(auto &frame:frames){assert(frame.frame&&frame.frame->width==320&&frame.frame->height==180);assert(frame.pts_us==unit.pts_us);av_frame_free(&frame.frame);}}
    assert(encoded==4&&decoded>=4);decoder.flush();capture.stop();unsetenv("OPAL_CAPTURE_CMD");return 0;
}
