#include <opal/media.hpp>
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

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
    std::cout<<"media tests passed\n";
}
