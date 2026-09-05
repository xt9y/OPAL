#include <opal/media.hpp>
#include <opal/input_record.hpp>
#include <opal/media_profile.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

namespace opal {
static std::string quote_arg(const std::string&s){std::string out="'";for(char c:s){if(c=='\'')out+="'\\''";else out+=c;}out+="'";return out;}
static void cloexec(int fd){int f=fcntl(fd,F_GETFD,0);if(f>=0)fcntl(fd,F_SETFD,f|FD_CLOEXEC);}
static void nonblock(int fd){int f=fcntl(fd,F_GETFL,0);if(f>=0)(void)fcntl(fd,F_SETFL,f|O_NONBLOCK);}
static void group_child(){setpgid(0,0);}static void group_parent(pid_t pid){if(pid>0)setpgid(pid,pid);}static void stop_group(pid_t pid){if(pid<=0)return;int status=0;pid_t rc=waitpid(pid,&status,WNOHANG);auto wait_until=[&](std::chrono::steady_clock::time_point deadline){while(rc==0&&std::chrono::steady_clock::now()<deadline){usleep(1000);rc=waitpid(pid,&status,WNOHANG);}};wait_until(std::chrono::steady_clock::now()+std::chrono::milliseconds(25));if(rc==0){(void)kill(-pid,SIGTERM);wait_until(std::chrono::steady_clock::now()+std::chrono::milliseconds(100));}if(rc==0){(void)kill(-pid,SIGKILL);while(waitpid(pid,&status,0)<0&&errno==EINTR){}};}
static std::string command_output(const char*command){if(!command||!*command)return{};FILE*pipe=popen(command,"r");if(!pipe)return{};std::string out;std::array<char,4096>buffer{};while(fgets(buffer.data(),static_cast<int>(buffer.size()),pipe))out+=buffer.data();pclose(pipe);return out;}
static bool gsr_ffmpeg_opts_supported(){static const bool supported=command_output("gpu-screen-recorder --help 2>&1").find("-ffmpeg-opts")!=std::string::npos;return supported;}
static bool ffmpeg_h264_encoder_usable(const char*encoder){if(!encoder||!*encoder)return false;std::string command="ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc=size=32x32:rate=1 -frames:v 1 -pix_fmt yuv420p -c:v "+std::string(encoder)+" -bf 0 -g 1 ";if(std::string(encoder)=="libx264")command+="-preset ultrafast -tune zerolatency -keyint_min 1 -sc_threshold 0 ";command+="-an -f flv - >/dev/null 2>&1";return std::system(command.c_str())==0;}
static std::string ffmpeg_h264_encoder(){static const std::string encoder=[](){const char*candidates[]={"libx264","libopenh264"};for(const char*name:candidates)if(ffmpeg_h264_encoder_usable(name))return std::string(name);return std::string{};}();return encoder;}
std::string capture_command(bool gsr,int fps,int bitrate,bool audio,const std::string &portal_token_file,int max_width,int max_height){fps=std::clamp(fps,15,240);bitrate=std::clamp(bitrate,1000,100000);const int gop=normal_gop_frames(fps);const auto stale_budget=capture_stale_budget_us(fps);const long long vbv_bits=static_cast<long long>(bitrate)*stale_budget;const int vbv_kbits=std::max(250,static_cast<int>((vbv_bits+999999LL)/1000000LL));if(max_width<0||max_height<0){max_width=0;max_height=0;}const std::string scale=std::to_string(max_width)+"x"+std::to_string(max_height);if(gsr){const char *wayland=std::getenv("WAYLAND_DISPLAY");const bool portal=wayland&&*wayland;const std::string source=portal?"portal":"screen";std::string portal_args;if(portal&&!portal_token_file.empty()){portal_args+=" -portal-session-token-filepath "+quote_arg(portal_token_file);if(std::filesystem::exists(portal_token_file))portal_args+=" -restore-portal-session yes";}const std::string mux_opts=gsr_ffmpeg_opts_supported()?" -ffmpeg-opts 'flush_packets=1'":"";return "gpu-screen-recorder -w "+source+" -f "+std::to_string(fps)+" -fm vfr -keyint 0.25 -k h264 -fallback-cpu-encoding yes -bm cbr -tune performance -q "+std::to_string(bitrate)+" -v no -cursor yes -s "+scale+(audio?" -a default_output":"")+portal_args+mux_opts+" -c flv";}std::string filter;if(max_width>0&&max_height>0)filter="-vf \"scale='min(iw,"+std::to_string(max_width)+")':'min(ih,"+std::to_string(max_height)+")':force_original_aspect_ratio=decrease\" ";const auto encoder=ffmpeg_h264_encoder();if(encoder.empty())return{};std::string encoder_args="-c:v "+encoder+" -bf 0 -g "+std::to_string(gop)+" ";if(encoder=="libx264")encoder_args+="-preset ultrafast -tune zerolatency -keyint_min "+std::to_string(gop)+" -sc_threshold 0 ";return "ffmpeg -hide_banner -loglevel error -fflags nobuffer -flags low_delay -f x11grab -draw_mouse 1 -framerate "+std::to_string(fps)+" -i ${DISPLAY:-:0.0} "+(audio?"-f pulse -i default ":"")+filter+encoder_args+"-b:v "+std::to_string(bitrate)+"k -maxrate "+std::to_string(bitrate)+"k -bufsize "+std::to_string(vbv_kbits)+"k "+(audio?"-c:a aac -b:a 128k ":"")+"-flush_packets 1 -f flv pipe:1";}
CaptureProcess start_capture(const std::string &command){if(command.empty())return{};int fds[2];if(pipe(fds)!=0)return{};cloexec(fds[0]);cloexec(fds[1]);pid_t pid=fork();if(pid<0){close(fds[0]);close(fds[1]);return{};}if(pid==0){group_child();dup2(fds[1],STDOUT_FILENO);close(fds[0]);close(fds[1]);execl("/bin/sh","sh","-c",command.c_str(),static_cast<char*>(nullptr));_exit(127);}group_parent(pid);close(fds[1]);return{pid,fds[0]};}
int read_capture(CaptureProcess &capture,void *buffer,size_t size,int timeout_ms){if(capture.fd<0||!buffer||size==0)return -1;pollfd pfd{capture.fd,static_cast<short>(POLLIN|POLLHUP|POLLERR),0};int rc;do{rc=poll(&pfd,1,timeout_ms);}while(rc<0&&errno==EINTR);if(rc==0)return -2;if(rc<0)return -1;ssize_t n;do{n=read(capture.fd,buffer,size);}while(n<0&&errno==EINTR);return n<0?-1:static_cast<int>(n);}
void stop_capture(CaptureProcess &capture){if(capture.fd>=0){close(capture.fd);capture.fd=-1;}stop_group(capture.pid);capture.pid=-1;}
SinkProcess start_sink(const std::string &command){int fds[2];if(pipe(fds)!=0)return{};cloexec(fds[0]);cloexec(fds[1]);pid_t pid=fork();if(pid<0){close(fds[0]);close(fds[1]);return{};}if(pid==0){group_child();dup2(fds[0],STDIN_FILENO);close(fds[0]);close(fds[1]);execl("/bin/sh","sh","-c",command.c_str(),static_cast<char*>(nullptr));_exit(127);}group_parent(pid);close(fds[0]);nonblock(fds[1]);const bool compact_input=command.find("opal-input")!=std::string::npos;return{pid,fds[1],compact_input};}
bool write_sink_timeout(SinkProcess &sink,const void *data,size_t size,int timeout_ms){
    if(sink.fd<0||(!data&&size))return false;
    std::array<std::uint8_t,kInputRecordBytes>compact{};
    if(sink.compact_input){const std::string_view command(static_cast<const char*>(data),size);if(!encode_input_command(command,compact))return false;data=compact.data();size=compact.size();timeout_ms=std::min(timeout_ms,2);}
    auto*p=static_cast<const unsigned char*>(data);auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(std::max(1,timeout_ms));while(size){auto now=std::chrono::steady_clock::now();if(now>=deadline)return false;auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count();pollfd f{sink.fd,POLLOUT,0};int rc=poll(&f,1,static_cast<int>(std::max<long long>(1,ms)));if(rc<0&&errno==EINTR)continue;if(rc<=0||f.revents&(POLLERR|POLLHUP|POLLNVAL))return false;ssize_t n=write(sink.fd,p,size);if(n<0&&errno==EINTR)continue;if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))continue;if(n<=0)return false;p+=n;size-=static_cast<size_t>(n);}return true;
}
void stop_sink(SinkProcess &sink){if(sink.fd>=0){close(sink.fd);sink.fd=-1;}stop_group(sink.pid);sink.pid=-1;sink.compact_input=false;}
}
