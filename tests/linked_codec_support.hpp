#pragma once

#include <iostream>
extern "C" {
#include <libavcodec/avcodec.h>
}

namespace opal_test {
inline bool linked_h264_decoder_available(){return avcodec_find_decoder(AV_CODEC_ID_H264)!=nullptr;}
inline bool require_linked_h264_decoder(){
    if(linked_h264_decoder_available())return true;
    std::cerr<<"SKIP decoder-dependent test: linked libavcodec has no H.264 decoder; on Fedora install RPM Fusion libavcodec-freeworld/ffmpeg-libs\n";
    return false;
}
}
