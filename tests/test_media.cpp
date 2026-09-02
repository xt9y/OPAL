#include <opal/media.hpp>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

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
        auto first=opal::capture_command(true,60,20000,true,token);
        assert(first.find("-w portal")!=std::string::npos);
        assert(first.find("-portal-session-token-filepath")!=std::string::npos);
        assert(first.find(token)!=std::string::npos);
        assert(first.find("-restore-portal-session yes")==std::string::npos);
        {std::ofstream f(token);f<<"restore-token";}
        auto restored=opal::capture_command(true,60,20000,true,token);
        assert(restored.find("-portal-session-token-filepath")!=std::string::npos);
        assert(restored.find("-restore-portal-session yes")!=std::string::npos);
        unlink(token.c_str());
        auto no_state=opal::capture_command(true,60,20000,true);
        assert(no_state.find("-portal-session-token-filepath")==std::string::npos);
        unsetenv("WAYLAND_DISPLAY");
    }
    std::cout<<"media tests passed\n";
}
