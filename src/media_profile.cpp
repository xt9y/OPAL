#include <opal/media_profile.hpp>
#include <algorithm>

namespace opal {
StreamOptions default_stream_options(){return {};}

bool stream_mode_limit(const std::string &mode,int &max_width,int &max_height){
    if(mode=="max"){max_width=0;max_height=0;return true;}
    if(mode=="1080p"){max_width=1920;max_height=1080;return true;}
    if(mode=="1440p"){max_width=2560;max_height=1440;return true;}
    if(mode=="4k"){max_width=3840;max_height=2160;return true;}
    return false;
}

int automatic_bitrate_kbps(int width,int height,int fps){
    fps=std::clamp(fps,15,240);
    if(width<=0||height<=0)return 60000;
    constexpr long long reference=1920LL*1080LL*60LL;
    long long pixel_rate=static_cast<long long>(width)*height*fps;
    long long bitrate=18000LL+(12000LL*pixel_rate)/reference;
    return static_cast<int>(std::clamp<long long>(bitrate,20000,100000));
}

std::uint64_t capture_stale_budget_us(int fps){
    fps=std::clamp(fps,15,240);
    return static_cast<std::uint64_t>(std::clamp(2000000/fps,20000,150000));
}

int normal_gop_frames(int fps){
    fps=std::clamp(fps,15,240);
    return fps*2;
}

std::uint64_t sender_burst_budget_bytes(int,int,bool){
    return 2ULL*1200ULL;
}

int sender_pacing_rate_kbps(int bitrate_kbps,bool keyframe){
    const long long bitrate=std::clamp<long long>(bitrate_kbps,1000,100000);
    if(keyframe)return static_cast<int>(bitrate*4);
    return static_cast<int>((bitrate*6+4)/5);
}
}
