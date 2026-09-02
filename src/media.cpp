#include <opal/media.hpp>
#include <algorithm>
namespace opal {
std::string capture_command(bool gsr,int fps,int bitrate,bool audio){fps=std::clamp(fps,15,240);bitrate=std::clamp(bitrate,1000,100000);if(gsr){return "gpu-screen-recorder -w " + std::string("${WAYLAND_DISPLAY:+portal}${WAYLAND_DISPLAY:-screen}") + " -f "+std::to_string(fps)+" -v h264 -bm cbr -q "+std::to_string(bitrate)+(audio?" -a default_output":"")+" -c flv -o -";}return "ffmpeg -hide_banner -loglevel error -f x11grab -draw_mouse 1 -framerate "+std::to_string(fps)+" -i ${DISPLAY:-:0.0} "+(audio?"-f pulse -i default ":"")+"-c:v libx264 -preset ultrafast -tune zerolatency -b:v "+std::to_string(bitrate)+"k -maxrate "+std::to_string(bitrate)+"k -bufsize "+std::to_string(bitrate)+"k "+(audio?"-c:a aac -b:a 128k ":"")+"-f flv pipe:1";}
}
