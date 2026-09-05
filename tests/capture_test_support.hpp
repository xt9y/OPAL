#pragma once

#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

namespace opal_test {

inline std::string ffmpeg_h264_encoder(){
    static const std::string encoder=[](){
        FILE*pipe=popen("ffmpeg -hide_banner -encoders 2>/dev/null","r");
        if(!pipe)return std::string{};
        std::string text;std::array<char,4096>buffer{};
        while(fgets(buffer.data(),static_cast<int>(buffer.size()),pipe))text+=buffer.data();
        pclose(pipe);
        for(const char*name:{"libx264","libopenh264"})if(text.find(std::string(" ")+name+" ")!=std::string::npos)return std::string(name);
        return std::string{};
    }();
    return encoder;
}

inline bool capture_tests_available(){
    if(!ffmpeg_h264_encoder().empty())return true;
    std::cerr<<"SKIP capture-dependent test: FFmpeg has no libx264/libopenh264 H.264 encoder\n";
    return false;
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
    if(audio)command+="-c:a aac -b:a 96k ";else command+="-an ";
    command+="-flush_packets 1 -f flv pipe:1";
    return command;
}

}
