#include <opal/media.hpp>
#include <cassert>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unistd.h>

namespace opal {
bool stream_mode_limit(const std::string&,int&,int&);
int automatic_bitrate_kbps(int,int,int);
std::uint64_t capture_stale_budget_us(int);
}

static std::string read_file(const char *path){std::ifstream f(path);return std::string((std::istreambuf_iterator<char>(f)),{});}

int main() {
    {
        auto capture=opal::start_capture("printf MEDIA");
        assert(capture.pid>0);
        assert(capture.fd>=0);
        int flags=fcntl(capture.fd,F_GETFD,0);assert(flags>=0);assert((flags&FD_CLOEXEC)!=0);
        char buf[16]{};
        int n=opal::read_capture(capture,buf,sizeof(buf),1000);
        assert(n==5);
        assert(std::string(buf,buf+n)=="MEDIA");
        opal::stop_capture(capture);
        assert(capture.pid<0);
        assert(capture.fd<0);
    }
    {
        auto capture=opal::start_capture("sleep 2");
        assert(capture.pid>0);
        char buf[8]{};
        assert(opal::read_capture(capture,buf,sizeof(buf),100)==-2);
        opal::stop_capture(capture);
    }
    {
        // Generic bounded process input remains for the privileged input helper.
        auto sink=opal::start_sink("cat >/dev/null");
        assert(sink.pid>0&&sink.fd>=0);
        std::string burst(32*1024,'I');
        assert(opal::write_sink_timeout(sink,burst.data(),burst.size(),1000));
        opal::stop_sink(sink);
    }
    {
        int w=-1,h=-1;
        assert(opal::stream_mode_limit("max",w,h)&&w==0&&h==0);
        assert(opal::stream_mode_limit("1080p",w,h)&&w==1920&&h==1080);
        assert(opal::stream_mode_limit("1440p",w,h)&&w==2560&&h==1440);
        assert(opal::stream_mode_limit("4k",w,h)&&w==3840&&h==2160);
        assert(!opal::stream_mode_limit("potato",w,h));
        assert(opal::automatic_bitrate_kbps(1920,1080,60)==30000);
        assert(opal::automatic_bitrate_kbps(2560,1440,60)>=39000);
        assert(opal::automatic_bitrate_kbps(3840,2160,60)==66000);
        assert(opal::automatic_bitrate_kbps(3840,2160,120)==100000);
        assert(opal::capture_stale_budget_us(60)>=32000&&opal::capture_stale_budget_us(60)<=34000);
    }
    {
        setenv("WAYLAND_DISPLAY","wayland-0",1);
        std::string token="/tmp/opal-portal-token-"+std::to_string(static_cast<long long>(getpid()));
        unlink(token.c_str());
        auto first=opal::capture_command(true,60,30000,true,token,1920,1080);
        assert(first.find("-w portal")!=std::string::npos);
        assert(first.find("-portal-session-token-filepath")!=std::string::npos);
        assert(first.find(token)!=std::string::npos);
        assert(first.find("-restore-portal-session yes")==std::string::npos);
        assert(first.find("-cursor yes")!=std::string::npos);
        assert(first.find("-s 1920x1080")!=std::string::npos);
        assert(first.find("-keyint 0.25")!=std::string::npos);
        assert(first.find("-tune performance")!=std::string::npos);
        assert(first.find("-c flv")!=std::string::npos); // internal capture container only
        {std::ofstream f(token);f<<"restore-token";}
        auto restored=opal::capture_command(true,60,30000,true,token,0,0);
        assert(restored.find("-restore-portal-session yes")!=std::string::npos);
        assert(restored.find("-s 0x0")!=std::string::npos);
        unlink(token.c_str());
        unsetenv("WAYLAND_DISPLAY");
    }
    {
        auto fallback=opal::capture_command(false,60,30000,true,"",1920,1080);
        assert(fallback.find("-draw_mouse 1")!=std::string::npos);
        assert(fallback.find("scale=")!=std::string::npos);
        assert(fallback.find("-bf 0")!=std::string::npos);
        assert(fallback.find("-g 15")!=std::string::npos);
        assert(fallback.find("-bufsize 1000k")!=std::string::npos);
        assert(fallback.find("-bufsize 30000k")==std::string::npos);
        assert(fallback.find("-f flv pipe:1")!=std::string::npos); // demuxed before UDP
    }
    {
        auto session=read_file("src/session.cpp");
        assert(session.find("ffplay")==std::string::npos);
        assert(session.find("VIDEO_PROFILE ")!=std::string::npos);
        assert(session.find("negotiate_client_direct_video")!=std::string::npos);
        auto host=read_file("src/host.cpp");
        assert(host.find("negotiate_host_direct_video")!=std::string::npos);
        assert(host.find("DIRECT_MEDIA_READY")!=std::string::npos);
        auto sender=read_file("src/video_sender.cpp");
        assert(sender.find("automatic_bitrate_kbps")!=std::string::npos);
        assert(sender.find("type==VideoMediaType::VideoH264")!=std::string::npos);
        assert(sender.find("fragment_media_unit")!=std::string::npos);
    }
    std::cout<<"media tests passed\n";
}
