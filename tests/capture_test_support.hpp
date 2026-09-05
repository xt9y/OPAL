#pragma once

#include <cstdlib>
#include <iostream>
#include <string>

namespace opal_test {

inline bool ffmpeg_h264_encoder_usable(const char*encoder){
    if(!encoder||!*encoder)return false;
    const std::string command="ffmpeg -hide_banner -loglevel error -f lavfi -i color=c=black:s=16x16:r=1 -frames:v 1 -pix_fmt yuv420p -c:v "+std::string(encoder)+" -an -f flv - >/dev/null 2>&1";
    return std::system(command.c_str())==0;
}

inline bool ffmpeg_aac_encoder_usable(){
    return std::system("ffmpeg -hide_banner -loglevel error -f lavfi -i anullsrc=r=48000:cl=stereo -frames:a 1 -c:a aac -f adts - >/dev/null 2>&1")==0;
}

inline std::string ffmpeg_h264_encoder(){
    static const std::string encoder=[](){
        const char*candidates[]={"libx264","libopenh264"};
        for(const char*name:candidates)if(ffmpeg_h264_encoder_usable(name))return std::string(name);
        return std::string{};
    }();
    return encoder;
}

inline bool capture_tests_available(bool audio=false){
    if(ffmpeg_h264_encoder().empty()){
        std::cerr<<"SKIP capture-dependent test: FFmpeg has no usable libx264/libopenh264 H.264 encoder\n";
        return false;
    }
    if(audio&&!ffmpeg_aac_encoder_usable()){
        std::cerr<<"SKIP capture-dependent test: FFmpeg AAC encoder is unavailable\n";
        return false;
    }
    return true;
}

inline std::string lavfi_video_command(int width,int height,int fps,int frames,int gop,bool realtime=false,bool audio=false){
    const auto encoder=ffmpeg_h264_encoder();if(encoder.empty())return{};
    std::string command="ffmpeg -hide_banner -loglevel error ";
    if(realtime)command+="-re ";
    command+="-f lavfi -i testsrc=size="+std::to_string(width)+"x"+std::to_string(height)+":rate="+std::to_string(fps)+" ";
    if(audio)command+="-f lavfi -i sine=frequency=440:sample_rate=48000 ";
    if(frames>0)command+="-frames:v "+std::to_string(frames)+" ";
    command+="-pix_fmt yuv420p -c:v "+encoder+" -bf 0 -g "+std::to_string(gop)+" ";
    if(encoder=="libx264")command+="-preset ultrafast -tune zerolatency -keyint_min "+std::to_string(gop)+" -sc_threshold 0 ";
    if(audio){command+="-c:a aac -b:a 96k ";if(frames>0)command+="-shortest ";}else command+="-an ";
    command+="-flush_packets 1 -f flv pipe:1";
    return command;
}

}
