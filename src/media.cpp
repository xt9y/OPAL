#include <opal/media.hpp>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace opal {
static std::string quote_arg(const std::string&s){std::string out="'";for(char c:s){if(c=='\'')out+="'\\''";else out+=c;}out+="'";return out;}
static void cloexec(int fd){int f=fcntl(fd,F_GETFD,0);if(f>=0)fcntl(fd,F_SETFD,f|FD_CLOEXEC);}
static void group_child(){setpgid(0,0);}
static void group_parent(pid_t pid){if(pid>0)setpgid(pid,pid);}
static void stop_group(pid_t pid){if(pid<=0)return;int status=0;pid_t rc=waitpid(pid,&status,WNOHANG);kill(-pid,SIGTERM);auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(500);while(rc==0&&std::chrono::steady_clock::now()<deadline){usleep(25000);rc=waitpid(pid,&status,WNOHANG);}kill(-pid,SIGKILL);if(rc==0)while(waitpid(pid,&status,0)<0&&errno==EINTR){};}

bool stream_mode_limit(const std::string &mode,int &max_width,int &max_height){
    if(mode=="max"){max_width=0;max_height=0;return true;}
    if(mode=="1080p"){max_width=1920;max_height=1080;return true;}
    if(mode=="1440p"){max_width=2560;max_height=1440;return true;}
    if(mode=="4k"){max_width=3840;max_height=2160;return true;}
    return false;
}

int automatic_bitrate_kbps(int width,int height,int fps){
    fps=std::clamp(fps,15,240);
    if(width<=0||height<=0)return 60000;
    constexpr long long reference=1920LL*1080LL*60LL;
    long long pixel_rate=static_cast<long long>(width)*height*fps;
    long long bitrate=18000LL+(12000LL*pixel_rate)/reference;
    return static_cast<int>(std::clamp<long long>(bitrate,20000,100000));
}

std::string video_request_line(const std::string &token,int max_width,int max_height,int fps){
    if(max_width==0&&max_height==0&&fps==60)return "VIDEO "+token;
    return "VIDEO "+token+" "+std::to_string(max_width)+" "+std::to_string(max_height)+" "+std::to_string(fps);
}

bool parse_video_request_line(const std::string &line,std::string &token,int &max_width,int &max_height,int &fps){
    std::istringstream ss(line);std::string word,extra;
    if(!(ss>>word>>token)||word!="VIDEO"||token.empty())return false;
    max_width=0;max_height=0;fps=60;
    if(ss>>max_width){
        if(!(ss>>max_height>>fps))return false;
        if(ss>>extra)return false;
    }else if(!ss.eof())return false;
    bool native=max_width==0&&max_height==0;
    bool limited=max_width>0&&max_height>0&&max_width<=7680&&max_height<=4320;
    return (native||limited)&&fps>=15&&fps<=240;
}

std::string capture_command(bool gsr,int fps,int bitrate,bool audio,const std::string &portal_token_file,int max_width,int max_height){
    fps=std::clamp(fps,15,240);bitrate=std::clamp(bitrate,1000,100000);
    if(max_width<0||max_height<0){max_width=0;max_height=0;}
    const std::string scale=std::to_string(max_width)+"x"+std::to_string(max_height);
    if(gsr){
        const char *wayland=std::getenv("WAYLAND_DISPLAY");const char *debug=std::getenv("OPAL_DEBUG");
        const bool portal=wayland&&*wayland;const std::string source=portal?"portal":"screen";
        const bool debug_enabled=debug&&*debug&&std::string(debug)!="0";
        std::string portal_args;
        if(portal&&!portal_token_file.empty()){
            portal_args+=" -portal-session-token-filepath "+quote_arg(portal_token_file);
            if(std::filesystem::exists(portal_token_file))portal_args+=" -restore-portal-session yes";
        }
        return "gpu-screen-recorder -w "+source+" -f "+std::to_string(fps)+" -fm cfr -keyint 1 -k h264 -fallback-cpu-encoding yes -bm cbr -q "+std::to_string(bitrate)+" -v no -cursor yes -s "+scale+(audio?" -a default_output":"")+portal_args+" -c flv"+(debug_enabled?"":" 2>/dev/null");
    }
    std::string filter;
    if(max_width>0&&max_height>0)filter="-vf \"scale='min(iw,"+std::to_string(max_width)+")':'min(ih,"+std::to_string(max_height)+")':force_original_aspect_ratio=decrease\" ";
    return "ffmpeg -hide_banner -loglevel error -f x11grab -draw_mouse 1 -framerate "+std::to_string(fps)+" -i ${DISPLAY:-:0.0} "+(audio?"-f pulse -i default ":"")+filter+"-c:v libx264 -preset ultrafast -tune zerolatency -bf 0 -g "+std::to_string(fps)+" -keyint_min "+std::to_string(fps)+" -sc_threshold 0 -b:v "+std::to_string(bitrate)+"k -maxrate "+std::to_string(bitrate)+"k -bufsize "+std::to_string(bitrate)+"k "+(audio?"-c:a aac -b:a 128k ":"")+"-f flv pipe:1";
}

CaptureProcess start_capture(const std::string &command){int fds[2];if(pipe(fds)!=0)return{};cloexec(fds[0]);cloexec(fds[1]);pid_t pid=fork();if(pid<0){close(fds[0]);close(fds[1]);return{};}if(pid==0){group_child();dup2(fds[1],STDOUT_FILENO);close(fds[0]);close(fds[1]);execl("/bin/sh","sh","-c",command.c_str(),static_cast<char*>(nullptr));_exit(127);}group_parent(pid);close(fds[1]);return{pid,fds[0]};}
int read_capture(CaptureProcess &capture,void *buffer,size_t size,int timeout_ms){if(capture.fd<0||!buffer||size==0)return -1;pollfd pfd{capture.fd,static_cast<short>(POLLIN|POLLHUP|POLLERR),0};int rc;do{rc=poll(&pfd,1,timeout_ms);}while(rc<0&&errno==EINTR);if(rc==0)return -2;if(rc<0)return -1;ssize_t n;do{n=read(capture.fd,buffer,size);}while(n<0&&errno==EINTR);return n<0?-1:static_cast<int>(n);}
void stop_capture(CaptureProcess &capture){if(capture.fd>=0){close(capture.fd);capture.fd=-1;}stop_group(capture.pid);capture.pid=-1;}

SinkProcess start_sink(const std::string &command){int fds[2];if(pipe(fds)!=0)return{};cloexec(fds[0]);cloexec(fds[1]);pid_t pid=fork();if(pid<0){close(fds[0]);close(fds[1]);return{};}if(pid==0){group_child();dup2(fds[0],STDIN_FILENO);close(fds[0]);close(fds[1]);execl("/bin/sh","sh","-c",command.c_str(),static_cast<char*>(nullptr));_exit(127);}group_parent(pid);close(fds[0]);return{pid,fds[1]};}
int video_player_write_timeout_ms(){return 2000;}
bool write_sink_timeout(SinkProcess &sink,const void *data,size_t size,int timeout_ms){if(sink.fd<0)return false;auto*p=static_cast<const unsigned char*>(data);auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(std::max(1,timeout_ms));while(size){auto now=std::chrono::steady_clock::now();if(now>=deadline)return false;auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count();pollfd f{sink.fd,POLLOUT,0};int rc=poll(&f,1,static_cast<int>(std::max<long long>(1,ms)));if(rc<0&&errno==EINTR)continue;if(rc<=0||f.revents&(POLLERR|POLLHUP|POLLNVAL))return false;ssize_t n=write(sink.fd,p,size);if(n<0&&errno==EINTR)continue;if(n<=0)return false;p+=n;size-=static_cast<size_t>(n);}return true;}
void stop_sink(SinkProcess &sink){if(sink.fd>=0){close(sink.fd);sink.fd=-1;}stop_group(sink.pid);sink.pid=-1;}
}
