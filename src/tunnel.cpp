#include <opal/tunnel.hpp>
#include <opal/config.hpp>
#include <opal/crypto.hpp>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include <signal.h>

namespace opal {
namespace {

pid_t spawn(const std::vector<std::string> &args) {
    pid_t pid=fork();
    if(pid==0) {
        std::vector<char*> argv;
        argv.reserve(args.size()+1);
        for(const auto &arg:args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0],argv.data());
        _exit(127);
    }
    return pid;
}

int run_wait(const std::vector<std::string> &args) {
    pid_t pid=spawn(args);
    if(pid<0) return 1;
    int status=0;
    if(waitpid(pid,&status,0)<0) return 1;
    return WIFEXITED(status)?WEXITSTATUS(status):1;
}

int run_capture(const std::vector<std::string> &args,std::string &output) {
    int fds[2];
    if(pipe(fds)!=0) return 1;
    pid_t pid=fork();
    if(pid==0) {
        close(fds[0]);
        dup2(fds[1],STDOUT_FILENO);
        dup2(fds[1],STDERR_FILENO);
        close(fds[1]);
        std::vector<char*> argv;
        argv.reserve(args.size()+1);
        for(const auto &arg:args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0],argv.data());
        _exit(127);
    }
    close(fds[1]);
    output.clear();
    char buffer[4096];
    for(;;) {
        ssize_t n=read(fds[0],buffer,sizeof(buffer));
        if(n<=0) break;
        output.append(buffer,static_cast<size_t>(n));
    }
    close(fds[0]);
    if(pid<0) return 1;
    int status=0;
    if(waitpid(pid,&status,0)<0) return 1;
    return WIFEXITED(status)?WEXITSTATUS(status):1;
}

bool zrok2_environment_enabled() {
    std::string status_output;
    if(run_capture({"zrok2","status"},status_output)!=0) {
        std::cerr<<status_output;
        std::cerr<<"Could not read zrok2 environment status\n";
        return false;
    }
    return status_output.find("EnvZId")!=std::string::npos;
}

bool ensure_zrok2() {
    if(!command_exists("zrok2")) {
        std::cerr<<"OPAL requires zrok2 for networking. Install zrok2, then run opal again.\n";
        return false;
    }
    if(zrok2_environment_enabled()) return true;
    std::cout<<"zrok2 is not enabled on this computer.\nZrok enable token: ";
    std::string token;
    if(!std::getline(std::cin,token)) return false;
    token=trim(token);
    if(token.empty()) return false;
    if(run_wait({"zrok2","enable",token})!=0) {
        std::cerr<<"zrok2 enable failed\n";
        return false;
    }
    if(!zrok2_environment_enabled()) {
        std::cerr<<"zrok2 enable completed but no enabled environment was found\n";
        return false;
    }
    return true;
}

bool create_persistent_share(const std::string &token) {
    return run_wait({"zrok2","create","share","--share-token",token,"--backend-mode","tcpTunnel"})==0;
}

bool child_started(pid_t pid) {
    if(pid<0) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    int status=0;
    pid_t rc=waitpid(pid,&status,WNOHANG);
    return rc==0;
}

std::string make_token(const char *kind) {
    return std::string("opal-")+kind+"-"+random_hex(5);
}

}

bool tunnel_connection_code(const std::string &code,std::string *control_token,std::string *video_token) {
    if(code.rfind("opal:",0)!=0) return false;
    auto body=code.substr(5);
    auto comma=body.find(',');
    if(comma==std::string::npos||comma==0||comma+1>=body.size()||body.find(',',comma+1)!=std::string::npos) return false;
    auto control=body.substr(0,comma);
    auto video=body.substr(comma+1);
    if(control.empty()||video.empty()) return false;
    if(control_token) *control_token=control;
    if(video_token) *video_token=video;
    return true;
}

int tunnel_host_setup(std::string &connection_code) {
    if(!ensure_zrok2()) return 2;
    auto paths=Paths::load();
    ensure_layout(paths);
    Ini host;
    host.load(paths.host);
    auto control=host.get("tunnel","control_token");
    auto video=host.get("tunnel","video_token");
    if(control.empty()) {
        for(int attempt=0;attempt<4&&control.empty();++attempt) {
            auto candidate=make_token("ctl");
            if(create_persistent_share(candidate)) control=candidate;
        }
    }
    if(video.empty()) {
        for(int attempt=0;attempt<4&&video.empty();++attempt) {
            auto candidate=make_token("vid");
            if(create_persistent_share(candidate)) video=candidate;
        }
    }
    if(control.empty()||video.empty()) {
        std::cerr<<"Could not allocate persistent zrok2 shares\n";
        return 1;
    }
    host.set("tunnel","control_token",control);
    host.set("tunnel","video_token",video);
    host.set("tunnel","mode","zrok2-private");
    if(!host.save(paths.host)) return 1;
    connection_code="opal:"+control+","+video;
    return 0;
}

int tunnel_host_start() {
    auto paths=Paths::load();
    Ini host;
    host.load(paths.host);
    auto control=host.get("tunnel","control_token");
    auto video=host.get("tunnel","video_token");
    if(control.empty()||video.empty()) {
        std::string code;
        int rc=tunnel_host_setup(code);
        if(rc!=0) return rc;
        host.load(paths.host);
        control=host.get("tunnel","control_token");
        video=host.get("tunnel","video_token");
        std::cout<<"OPAL connection code: "<<code<<"\n";
    } else if(!ensure_zrok2()) return 2;
    auto control_pid=spawn({"zrok2","share","private","--headless","--share-token",control,"127.0.0.1:47990"});
    auto video_pid=spawn({"zrok2","share","private","--headless","--share-token",video,"127.0.0.1:47991"});
    if(!child_started(control_pid)||!child_started(video_pid)) {
        if(control_pid>0) kill(control_pid,SIGTERM);
        if(video_pid>0) kill(video_pid,SIGTERM);
        std::cerr<<"Could not start OPAL zrok2 tunnel\n";
        return 1;
    }
    return 0;
}

bool tunnel_access(const std::string &control_token,const std::string &video_token) {
    if(!ensure_zrok2()) return false;
    auto control_pid=spawn({"zrok2","access","private",control_token,"--bind","127.0.0.1:47990","--headless"});
    auto video_pid=spawn({"zrok2","access","private",video_token,"--bind","127.0.0.1:47991","--headless"});
    if(!child_started(control_pid)||!child_started(video_pid)) {
        if(control_pid>0) kill(control_pid,SIGTERM);
        if(video_pid>0) kill(video_pid,SIGTERM);
        return false;
    }
    return true;
}

int tunnel_host() {
    std::string code;
    int rc=tunnel_host_setup(code);
    if(rc!=0) return rc;
    std::cout<<"OPAL connection code: "<<code<<"\n";
    rc=tunnel_host_start();
    if(rc!=0) return rc;
    std::cout<<"Tunnel running. Keep this process alive.\n";
    for(;;) pause();
}

}
