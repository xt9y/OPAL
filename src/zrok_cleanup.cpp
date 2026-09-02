#include <opal/zrok_cleanup.hpp>
#include <opal/config.hpp>
#include <opal/tunnel.hpp>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <iostream>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace opal {
namespace {
void cloexec(int fd) {
    int flags=fcntl(fd,F_GETFD,0);
    if(flags>=0) (void)fcntl(fd,F_SETFD,flags|FD_CLOEXEC);
}

int wait_child(pid_t pid) {
    if(pid<0) return 1;
    int status=0;
    while(waitpid(pid,&status,0)<0) {
        if(errno==EINTR) continue;
        return 1;
    }
    return WIFEXITED(status)?WEXITSTATUS(status):1;
}

int run_wait(const std::vector<std::string> &args) {
    pid_t pid=fork();
    if(pid==0) {
        int nullfd=open("/dev/null",O_WRONLY|O_CLOEXEC);
        if(nullfd>=0) {
            dup2(nullfd,STDOUT_FILENO);
            dup2(nullfd,STDERR_FILENO);
            if(nullfd>STDERR_FILENO) close(nullfd);
        }
        std::vector<char*> argv;
        argv.reserve(args.size()+1);
        for(const auto &arg:args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0],argv.data());
        _exit(127);
    }
    return wait_child(pid);
}

int run_capture(const std::vector<std::string> &args,std::string &output) {
    int fds[2];
    if(pipe(fds)!=0) return 1;
    cloexec(fds[0]);cloexec(fds[1]);
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
    if(pid<0) { close(fds[0]);return 1; }
    output.clear();
    char buffer[4096];
    for(;;) {
        ssize_t n=read(fds[0],buffer,sizeof(buffer));
        if(n>0) { output.append(buffer,static_cast<size_t>(n));continue; }
        if(n<0&&errno==EINTR) continue;
        break;
    }
    close(fds[0]);
    return wait_child(pid);
}

void add_connection_tokens(std::set<std::string> &tokens,const std::string &address) {
    std::string control,video;
    if(tunnel_connection_code(address,&control,&video)) {
        tokens.insert(control);tokens.insert(video);return;
    }
    if(address.rfind("zrok:",0)!=0) return;
    auto body=address.substr(5);
    auto comma=body.find(',');
    if(comma==std::string::npos||comma==0||comma+1>=body.size()) return;
    control=body.substr(0,comma);video=body.substr(comma+1);
    if(!control.empty()) tokens.insert(control);
    if(!video.empty()) tokens.insert(video);
}

std::set<std::string> known_share_tokens() {
    auto paths=Paths::load();
    std::set<std::string> tokens;
    Ini host;
    if(host.load(paths.host)) {
        auto control=host.get("tunnel","control_token");
        auto video=host.get("tunnel","video_token");
        if(!control.empty()) tokens.insert(control);
        if(!video.empty()) tokens.insert(video);
    }
    Ini hosts;
    if(hosts.load(paths.hosts)) {
        for(const auto &[name,values]:hosts.sections()) {
            (void)name;
            auto it=values.find("address");
            if(it!=values.end()) add_connection_tokens(tokens,it->second);
        }
    }
    return tokens;
}

std::set<std::string> frontend_tokens(const std::string &json) {
    static const std::regex pattern(R"json("frontendToken"\s*:\s*"([^"]+)")json");
    std::set<std::string> result;
    for(std::sregex_iterator it(json.begin(),json.end(),pattern),end;it!=end;++it)
        if((*it).size()>1&&!(*it)[1].str().empty()) result.insert((*it)[1].str());
    return result;
}
}

int clean_zrok_accesses() {
    auto shares=known_share_tokens();
    if(shares.empty()) return 0;
    if(!command_exists("zrok2")) {
        std::cerr<<"Could not clean OPAL zrok accesses: zrok2 is not installed.\n";
        return 1;
    }

    int rc=0;
    std::set<std::string> deleted;
    for(const auto &share:shares) {
        std::string json;
        if(run_capture({"zrok2","list","accesses","--share-token",share,"--json"},json)!=0) {
            std::cerr<<"Warning: could not list zrok accesses for "<<share<<"\n";
            rc=1;
            continue;
        }
        for(const auto &frontend:frontend_tokens(json)) {
            if(!deleted.insert(frontend).second) continue;
            if(run_wait({"zrok2","delete","access",frontend})!=0) {
                std::cerr<<"Warning: could not delete zrok access "<<frontend<<"\n";
                rc=1;
            }
        }
    }
    return rc;
}
}
