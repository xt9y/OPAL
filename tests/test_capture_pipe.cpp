#include "capture_test_support.hpp"
#include <opal/media.hpp>

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

int main(){
    if(!opal_test::capture_tests_available())return 0;
    const auto command=opal_test::lavfi_video_command(160,90,30,30,8,false,false);
    assert(!command.empty());

    auto capture=opal::start_capture(command);
    assert(capture.pid>0&&capture.fd>=0);

    std::array<std::uint8_t,4096> chunk{};
    std::vector<std::uint8_t> prefix;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    std::size_t bytes=0;
    while(std::chrono::steady_clock::now()<deadline&&bytes<13){
        const int n=opal::read_capture(capture,chunk.data(),chunk.size(),250);
        if(n==-2)continue;
        if(n<=0)break;
        bytes+=static_cast<std::size_t>(n);
        const auto needed=std::min<std::size_t>(13-prefix.size(),static_cast<std::size_t>(n));
        prefix.insert(prefix.end(),chunk.begin(),chunk.begin()+static_cast<std::ptrdiff_t>(needed));
    }
    opal::stop_capture(capture);

    if(prefix.size()<13){std::cerr<<"capture pipe produced only "<<bytes<<" bytes before EOF/deadline\n";return 1;}
    assert(prefix[0]=='F'&&prefix[1]=='L'&&prefix[2]=='V'&&prefix[3]==1);
    assert(prefix[5]==0&&prefix[6]==0&&prefix[7]==0&&prefix[8]==9);
    return 0;
}
