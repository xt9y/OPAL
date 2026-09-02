#include <opal/media.hpp>
#include <algorithm>
#include <cstdlib>
#include <string>

namespace opal {
std::string capture_command(bool gsr,int fps,int bitrate,bool audio){
    fps=std::clamp(fps,15,240);
    bitrate=std::clamp(bitrate,1000,100000);
    if(gsr){
        const char *wayland=std::getenv("WAYLAND_DISPLAY");
        const char *debug=std::getenv("OPAL_DEBUG");
        const std::string source=(wayland&&*wayland)?"portal":"screen";
        const bool debug_enabled=debug&&*debug&&std::string(debug)!="0";
        return "gpu-screen-recorder -w "+source+
            " -f "+std::to_string(fps)+
            " -k h264 -bm cbr -q "+std::to_string(bitrate)+
            " -v no"+
            (audio?" -a default_output":"")+
            " -c flv -o -"+
            (debug_enabled?"":" 2>/dev/null");
    }
    return "ffmpeg -hide_banner -loglevel error -f x11grab -draw_mouse 1 -framerate "+std::to_string(fps)+" -i ${DISPLAY:-:0.0} "+(audio?"-f pulse -i default ":"")+"-c:v libx264 -preset ultrafast -tune zerolatency -b:v "+std::to_string(bitrate)+"k -maxrate "+std::to_string(bitrate)+"k -bufsize "+std::to_string(bitrate)+"k "+(audio?"-c:a aac -b:a 128k ":"")+"-f flv pipe:1";
}
}
