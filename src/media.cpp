#include <opal/media.hpp>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <poll.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace opal {
static std::string quote_arg(const std::string&s){std::string out="'";for(char c:s){if(c=='\'')out+="'\\''";else out+=c;}out+="'";return out;}

std::string capture_command(bool gsr,int fps,int bitrate,bool audio,const std::string &portal_token_file){
    fps=std::clamp(fps,15,240);
    bitrate=std::clamp(bitrate,1000,100000);
    if(gsr){
        const char *wayland=std::getenv("WAYLAND_DISPLAY");
        const char *debug=std::getenv("OPAL_DEBUG");
        const bool portal=wayland&&*wayland;
        const std::string source=portal?"portal":"screen";
        const bool debug_enabled=debug&&*debug&&std::string(debug)!="0";
        std::string portal_args;
        if(portal&&!portal_token_file.empty()){
            portal_args+=" -portal-session-token-filepath "+quote_arg(portal_token_file);
            if(std::filesystem::exists(portal_token_file))portal_args+=" -restore-portal-session yes";
        }
        return "gpu-screen-recorder -w "+source+
            " -f "+std::to_string(fps)+
            " -fm cfr -keyint 1"
            " -k h264 -fallback-cpu-encoding yes -bm cbr -q "+std::to_string(bitrate)+
            " -v no -cursor yes"+
            (audio?" -a default_output":"")+
            portal_args+
            " -c mkv"+
            (debug_enabled?"":" 2>/dev/null");
    }
    return "ffmpeg -hide_banner -loglevel error -f x11grab -draw_mouse 1 -framerate "+std::to_string(fps)+" -i ${DISPLAY:-:0.0} "+(audio?"-f pulse -i default ":"")+"-c:v libx264 -preset ultrafast -tune zerolatency -b:v "+std::to_string(bitrate)+"k -maxrate "+std::to_string(bitrate)+"k -bufsize "+std::to_string(bitrate)+"k "+(audio?"-c:a aac -b:a 128k ":"")+"-f matroska pipe:1";
}

CaptureProcess start_capture(const std::string &command){
    int pipefd[2];
    if(pipe(pipefd)!=0) return {};
    pid_t pid=fork();
    if(pid<0){close(pipefd[0]);close(pipefd[1]);return {};}
    if(pid==0){
        dup2(pipefd[1],STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execl("/bin/sh","sh","-c",command.c_str(),static_cast<char*>(nullptr));
        _exit(127);
    }
    close(pipefd[1]);
    return {pid,pipefd[0]};
}

int read_capture(CaptureProcess &capture,void *buffer,size_t size,int timeout_ms){
    if(capture.fd<0||!buffer||size==0) return -1;
    pollfd pfd{};
    pfd.fd=capture.fd;
    pfd.events=POLLIN|POLLHUP|POLLERR;
    int rc;
    do { rc=poll(&pfd,1,timeout_ms); } while(rc<0&&errno==EINTR);
    if(rc==0) return -2;
    if(rc<0) return -1;
    ssize_t n;
    do { n=read(capture.fd,buffer,size); } while(n<0&&errno==EINTR);
    if(n<0) return -1;
    return static_cast<int>(n);
}

void stop_capture(CaptureProcess &capture){
    if(capture.fd>=0){close(capture.fd);capture.fd=-1;}
    if(capture.pid<=0){capture.pid=-1;return;}
    int status=0;
    pid_t rc=waitpid(capture.pid,&status,WNOHANG);
    if(rc==0){
        kill(capture.pid,SIGTERM);
        for(int i=0;i<10;i++){
            rc=waitpid(capture.pid,&status,WNOHANG);
            if(rc==capture.pid) break;
            usleep(50000);
        }
        if(rc==0){kill(capture.pid,SIGKILL);while(waitpid(capture.pid,&status,0)<0&&errno==EINTR){}}
    }
    capture.pid=-1;
}
}
