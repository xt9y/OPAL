#include <opal/media.hpp>
#include <cassert>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unistd.h>

namespace opal {
bool stream_mode_limit(const std::string&,int&,int&);
int automatic_bitrate_kbps(int,int,int);
std::string video_request_line(const std::string&,int,int,int);
bool parse_video_request_line(const std::string&,std::string&,int&,int&,int&);
std::string capture_command(bool,int,int,bool,const std::string&,int,int);
int video_player_write_timeout_ms();
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
        // ffplay can take a few hundred milliseconds to start reading stdin.
        // A live stream must tolerate that startup stall instead of tearing
        // down the TLS/video session as soon as the pipe fills.
        auto sink=opal::start_sink("sleep 0.35; cat >/dev/null");
        assert(sink.pid>0&&sink.fd>=0);
        std::string burst(256*1024,'V');
        assert(opal::video_player_write_timeout_ms()>=1000);
        assert(opal::write_sink_timeout(sink,burst.data(),burst.size(),opal::video_player_write_timeout_ms()));
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
        assert(opal::video_request_line("token",2560,1440,120)=="VIDEO token 2560 1440 120");
        std::string token;int fps=0;
        assert(opal::parse_video_request_line("VIDEO token 1920 1080 30",token,w,h,fps));
        assert(token=="token"&&w==1920&&h==1080&&fps==30);
        assert(opal::parse_video_request_line("VIDEO legacy",token,w,h,fps));
        assert(token=="legacy"&&w==0&&h==0&&fps==60);
        assert(!opal::parse_video_request_line("VIDEO token 9000 9000 60",token,w,h,fps));
        assert(!opal::parse_video_request_line("VIDEO token 1920 1080 0",token,w,h,fps));
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
        assert(first.find("-c flv")!=std::string::npos);
        assert(first.find("-c mkv")==std::string::npos);
        {std::ofstream f(token);f<<"restore-token";}
        auto restored=opal::capture_command(true,60,30000,true,token,0,0);
        assert(restored.find("-portal-session-token-filepath")!=std::string::npos);
        assert(restored.find("-restore-portal-session yes")!=std::string::npos);
        assert(restored.find("-s 0x0")!=std::string::npos);
        unlink(token.c_str());
        auto no_state=opal::capture_command(true,60,30000,true,"",0,0);
        assert(no_state.find("-portal-session-token-filepath")==std::string::npos);
        unsetenv("WAYLAND_DISPLAY");
    }
    {
        auto fallback=opal::capture_command(false,60,30000,true,"",1920,1080);
        assert(fallback.find("-draw_mouse 1")!=std::string::npos);
        assert(fallback.find("scale=")!=std::string::npos);
        assert(fallback.find("1920")!=std::string::npos);
        assert(fallback.find("1080")!=std::string::npos);
        assert(fallback.find("-f flv pipe:1")!=std::string::npos);
        assert(fallback.find("-f matroska pipe:1")==std::string::npos);
    }
    {
        auto host=read_file("src/host.cpp");
        assert(host.find("automatic_bitrate_kbps")!=std::string::npos);
        auto session=read_file("src/session.cpp");
        assert(session.find("video_player_write_timeout_ms()")!=std::string::npos);
        assert(session.find("-fflags nobuffer")!=std::string::npos);
        assert(session.find("-avioflags direct")!=std::string::npos);
        assert(session.find("-probesize 32")!=std::string::npos);
        assert(session.find("-analyzeduration 0")!=std::string::npos);
        assert(session.find("-sync video")!=std::string::npos);
    }
    std::cout<<"media tests passed\n";
}
