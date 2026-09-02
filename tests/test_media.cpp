#include <opal/media.hpp>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unistd.h>

static std::string read_file(const char *path){std::ifstream f(path);return std::string((std::istreambuf_iterator<char>(f)),{});}

int main() {
    {
        auto capture=opal::start_capture("printf MEDIA");
        assert(capture.pid>0);
        assert(capture.fd>=0);
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
        setenv("WAYLAND_DISPLAY","wayland-0",1);
        std::string token="/tmp/opal-portal-token-"+std::to_string(static_cast<long long>(getpid()));
        unlink(token.c_str());
        auto first=opal::capture_command(true,60,12000,true,token);
        assert(first.find("-w portal")!=std::string::npos);
        assert(first.find("-portal-session-token-filepath")!=std::string::npos);
        assert(first.find(token)!=std::string::npos);
        assert(first.find("-restore-portal-session yes")==std::string::npos);
        assert(first.find("-cursor yes")!=std::string::npos);
        assert(first.find("-c mkv")!=std::string::npos);
        assert(first.find("-c flv")==std::string::npos);
        {std::ofstream f(token);f<<"restore-token";}
        auto restored=opal::capture_command(true,60,12000,true,token);
        assert(restored.find("-portal-session-token-filepath")!=std::string::npos);
        assert(restored.find("-restore-portal-session yes")!=std::string::npos);
        unlink(token.c_str());
        auto no_state=opal::capture_command(true,60,12000,true);
        assert(no_state.find("-portal-session-token-filepath")==std::string::npos);
        unsetenv("WAYLAND_DISPLAY");
    }
    {
        auto fallback=opal::capture_command(false,60,12000,true);
        assert(fallback.find("-draw_mouse 1")!=std::string::npos);
        assert(fallback.find("-f matroska pipe:1")!=std::string::npos);
        assert(fallback.find("-f flv pipe:1")==std::string::npos);
    }
    {
        auto host=read_file("src/host.cpp");
        assert(host.find("get_int(\"video\",\"bitrate_kbps\",12000)")!=std::string::npos);
        assert(host.find("cfg.get(\"video\",\"bitrate_kbps\",\"12000\")")!=std::string::npos);
    }
    std::cout<<"media tests passed\n";
}
