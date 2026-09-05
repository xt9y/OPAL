#include <opal/input_record.hpp>
#include <opal/media.hpp>
#include <array>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <unistd.h>

namespace opal {bool stream_mode_limit(const std::string&,int&,int&);int automatic_bitrate_kbps(int,int,int);std::uint64_t capture_stale_budget_us(int);int normal_gop_frames(int);}

static std::vector<std::uint8_t> read_binary(const std::string&path){std::ifstream in(path,std::ios::binary);return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),{});}

int main(){
    {
        auto capture=opal::start_capture("printf MEDIA");
        assert(capture.pid>0&&capture.fd>=0);
        int flags=fcntl(capture.fd,F_GETFD,0);assert(flags>=0&&((flags&FD_CLOEXEC)!=0));
        char buf[16]{};int n=opal::read_capture(capture,buf,sizeof(buf),1000);
        assert(n==5&&std::string(buf,buf+n)=="MEDIA");
        opal::stop_capture(capture);assert(capture.pid<0&&capture.fd<0);
    }
    {
        auto capture=opal::start_capture("sleep 2");assert(capture.pid>0);
        char buf[8]{};assert(opal::read_capture(capture,buf,sizeof(buf),100)==-2);
        opal::stop_capture(capture);
    }
    {
        auto sink=opal::start_sink("cat >/dev/null");
        assert(sink.pid>0&&sink.fd>=0&&!sink.compact_input);
        const int flags=fcntl(sink.fd,F_GETFL,0);assert(flags>=0&&(flags&O_NONBLOCK));
        std::string burst(32*1024,'I');assert(opal::write_sink_timeout(sink,burst.data(),burst.size(),1000));
        opal::stop_sink(sink);
    }
    {
        const std::string path="/tmp/opal-input-record-"+std::to_string(static_cast<long long>(getpid()))+".bin";
        unlink(path.c_str());
        auto sink=opal::start_sink("cat > "+path+" # opal-input");
        assert(sink.pid>0&&sink.fd>=0&&sink.compact_input);
        const std::string command="MOUSE -17 23\n";
        assert(opal::write_sink_timeout(sink,command.data(),command.size(),1000));
        opal::stop_sink(sink);
        const auto bytes=read_binary(path);unlink(path.c_str());
        assert(bytes.size()==opal::kInputRecordBytes);
        opal::InputRecord record{};assert(opal::decode_input_record(bytes,record));
        assert(record.type==opal::InputRecordType::Relative&&record.a==-17&&record.b==23);
    }
    {
        auto sink=opal::start_sink("sleep 5 # opal-input");assert(sink.pid>0&&sink.fd>=0&&sink.compact_input);
        std::array<char,4096>fill{};for(;;){const auto n=write(sink.fd,fill.data(),fill.size());if(n>0)continue;if(n<0&&errno==EINTR)continue;assert(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK));break;}
        const std::string command="POINTER 100 200\n";const auto begin=std::chrono::steady_clock::now();assert(!opal::write_sink_timeout(sink,command.data(),command.size(),1000));const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-begin).count();assert(elapsed<50);opal::stop_sink(sink);
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
        assert(opal::normal_gop_frames(60)==15);
    }
    {
        setenv("WAYLAND_DISPLAY","wayland-0",1);
        const std::string token="/tmp/opal-portal-token-"+std::to_string(static_cast<long long>(getpid()));
        unlink(token.c_str());
        auto first=opal::capture_command(true,60,30000,true,token,1920,1080);
        assert(first.find("-w portal")!=std::string::npos);
        assert(first.find("-portal-session-token-filepath")!=std::string::npos);
        assert(first.find(token)!=std::string::npos);
        assert(first.find("-restore-portal-session yes")==std::string::npos);
        assert(first.find("-cursor yes")!=std::string::npos);
        assert(first.find("-cursor no")==std::string::npos);
        assert(first.find("-s 1920x1080")!=std::string::npos);
        assert(first.find("-fm vfr")!=std::string::npos);
        assert(first.find("-fm cfr")==std::string::npos);
        assert(first.find("-keyint 0.25")!=std::string::npos);
        assert(first.find("-tune performance")!=std::string::npos);
        assert(first.find("-ffmpeg-opts 'flush_packets=1'")!=std::string::npos);
        assert(first.find("-c flv")!=std::string::npos);
        {std::ofstream f(token);f<<"restore-token";}
        auto restored=opal::capture_command(true,60,30000,true,token,0,0);
        assert(restored.find("-restore-portal-session yes")!=std::string::npos);
        assert(restored.find("-s 0x0")!=std::string::npos);
        unlink(token.c_str());unsetenv("WAYLAND_DISPLAY");
    }
    {
        auto fallback=opal::capture_command(false,60,30000,true,"",1920,1080);
        assert(fallback.find("-draw_mouse 1")!=std::string::npos);
        assert(fallback.find("scale=")!=std::string::npos);
        assert(fallback.find("-fflags nobuffer")!=std::string::npos);
        assert(fallback.find("-flags low_delay")!=std::string::npos);
        assert(fallback.find("-bf 0")!=std::string::npos);
        assert(fallback.find("-g 15")!=std::string::npos);
        assert(fallback.find("-keyint_min 15")!=std::string::npos);
        assert(fallback.find("-bufsize 1000k")!=std::string::npos);
        assert(fallback.find("-flush_packets 1")!=std::string::npos);
        assert(fallback.find("-f flv pipe:1")!=std::string::npos);
    }
    return 0;
}
