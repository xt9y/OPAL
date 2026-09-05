#pragma once

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

namespace opal_test {

inline bool command_emits_flv(const std::string&command){
    FILE*pipe=popen((command+" 2>/dev/null").c_str(),"r");
    if(!pipe)return false;
    std::array<unsigned char,8192> buffer{};
    std::array<unsigned char,13> prefix{};
    std::size_t prefix_size=0,total_bytes=0;
    bool read_error=false;
    for(;;){
        const std::size_t n=fread(buffer.data(),1,buffer.size(),pipe);
        if(n){
            total_bytes+=n;
            const std::size_t take=std::min<std::size_t>(prefix.size()-prefix_size,n);
            std::copy_n(buffer.data(),take,prefix.data()+prefix_size);
            prefix_size+=take;
            continue;
        }
        if(feof(pipe))break;
        if(ferror(pipe)){read_error=true;break;}
    }
    const int rc=pclose(pipe);
    return !read_error&&rc==0&&total_bytes>=64&&prefix_size>=13&&prefix[0]=='F'&&prefix[1]=='L'&&prefix[2]=='V'&&prefix[3]==1&&prefix[5]==0&&prefix[6]==0&&prefix[7]==0&&prefix[8]==9;
}

inline bool ffmpeg_h264_encoder_usable(const char*encoder){
    if(!encoder||!*encoder)return false;
    std::string command="ffmpeg -nostdin -hide_banner -loglevel error -f lavfi -i testsrc=size=320x180:rate=60 -frames:v 8 -pix_fmt yuv420p -c:v "+std::string(encoder)+" -bf 0 -g 4 ";
    if(std::string(encoder)=="libx264")command+="-preset ultrafast -tune zerolatency -keyint_min 4 -sc_threshold 0 ";
    command+="-b:v 4000k -maxrate 4000k -bufsize 1000k -an -flush_packets 1 -f flv pipe:1";
    return command_emits_flv(command);
}

inline bool ffmpeg_aac_encoder_usable(){
    return std::system("ffmpeg -nostdin -hide_banner -loglevel error -f lavfi -i anullsrc=r=48000:cl=stereo -frames:a 4 -c:a aac -f adts - >/dev/null 2>&1")==0;
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
        std::cerr<<"SKIP capture-dependent test: FFmpeg has no H.264 encoder that emits a valid streaming FLV\n";
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
    // These are test fixtures. Tests intentionally close capture pipes early in
    // several cases, so keep FFmpeg silent rather than printing expected EPIPE
    // diagnostics after OPAL has already consumed the required media.
    std::string command="ffmpeg -nostdin -hide_banner -loglevel quiet ";
    if(realtime)command+="-re ";
    command+="-f lavfi -i testsrc=size="+std::to_string(width)+"x"+std::to_string(height)+":rate="+std::to_string(fps)+" ";
    if(audio)command+="-f lavfi -i sine=frequency=440:sample_rate=48000 ";
    if(frames>0)command+="-frames:v "+std::to_string(frames)+" ";
    command+="-pix_fmt yuv420p -c:v "+encoder+" -bf 0 -g "+std::to_string(gop)+" ";
    if(encoder=="libx264")command+="-preset ultrafast -tune zerolatency -keyint_min "+std::to_string(gop)+" -sc_threshold 0 ";
    command+="-b:v 4000k -maxrate 4000k -bufsize 1000k ";
    if(audio){command+="-c:a aac -b:a 96k ";if(frames>0)command+="-shortest ";}else command+="-an ";
    command+="-flush_packets 1 -f flv pipe:1";
    return command;
}

}
