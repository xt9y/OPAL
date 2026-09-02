#include <opal/tunnel.hpp>
#include <opal/config.hpp>
#include <opal/crypto.hpp>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>
#include <signal.h>

namespace opal {
namespace {

bool debug_enabled() {
    const char *value=std::getenv("OPAL_DEBUG");
    if(!value||!*value) return false;
    std::string v=value;
    return v!="0"&&v!="false"&&v!="FALSE"&&v!="off"&&v!="OFF";
}

pid_t spawn(const std::vector<std::string> &args,bool quiet=false) {
    pid_t pid=fork();
    if(pid==0) {
        if(quiet&&!debug_enabled()) {
            int nullfd=open("/dev/null",O_WRONLY);
            if(nullfd>=0) {
                dup2(nullfd,STDOUT_FILENO);
                dup2(nullfd,STDERR_FILENO);
                if(nullfd>STDERR_FILENO) close(nullfd);
            }
        }
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
    pid_t pid=spawn(args,true);
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
        if(debug_enabled()) std::cerr<<status_output;
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
    std::cout<<R"(zrok2 is not enabled.

1. Open https://myzrok.io and sign in.
2. If prompted, choose "Link zrok Account".
3. In the bottom-left of the sidebar, click "zrok", then sign in there.
4. Click "Get Started" in the top-right.
5. Find the command starting with "zrok enable" and copy only the code/token after it.
6. Paste that token below.

Zrok enable token: )";
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

bool child_alive(pid_t &pid) {
    if(pid<0) return false;
    int status=0;
    pid_t rc=waitpid(pid,&status,WNOHANG);
    if(rc==0) return true;
    if(rc==pid) pid=-1;
    return false;
}

bool local_port_ready(uint16_t port) {
    int fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd<0) return false;
    sockaddr_in addr{};
    addr.sin_family=AF_INET;
    addr.sin_port=htons(port);
    inet_pton(AF_INET,"127.0.0.1",&addr.sin_addr);
    bool ready=connect(fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0;
    close(fd);
    return ready;
}

bool wait_for_access_endpoints(pid_t &control_pid,pid_t &video_pid) {
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    bool control_ready=false;
    bool video_ready=false;
    while(std::chrono::steady_clock::now()<deadline) {
        if(!child_alive(control_pid)||!child_alive(video_pid)) return false;
        if(!control_ready) control_ready=local_port_ready(47990);
        if(!video_ready) video_ready=local_port_ready(47991);
        if(control_ready&&video_ready) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

unsigned long long process_start_time(pid_t pid) {
    std::ifstream in("/proc/"+std::to_string(pid)+"/stat");
    std::string line;
    if(!std::getline(in,line)) return 0;
    auto end=line.rfind(')');
    if(end==std::string::npos||end+2>=line.size()) return 0;
    std::istringstream fields(line.substr(end+2));
    std::string value;
    for(int field=3;field<=22;++field) {
        if(!(fields>>value)) return 0;
        if(field==22) {
            try { return std::stoull(value); }
            catch(...) { return 0; }
        }
    }
    return 0;
}

std::vector<std::string> process_args(pid_t pid) {
    std::ifstream in("/proc/"+std::to_string(pid)+"/cmdline",std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
    std::vector<std::string> args;
    size_t pos=0;
    while(pos<raw.size()) {
        auto end=raw.find('\0',pos);
        if(end==std::string::npos) end=raw.size();
        if(end>pos) args.emplace_back(raw.substr(pos,end-pos));
        pos=end+1;
    }
    return args;
}

bool basename_is_zrok2(const std::string &arg) {
    return std::filesystem::path(arg).filename()=="zrok2";
}

bool has_opal_endpoint(const std::vector<std::string> &args) {
    for(const auto &arg:args) {
        if(arg=="127.0.0.1:47990"||arg=="127.0.0.1:47991") return true;
    }
    return false;
}

bool is_zrok_private_process(pid_t pid,bool opal_endpoints_only) {
    auto args=process_args(pid);
    for(size_t i=0;i+2<args.size();++i) {
        if(!basename_is_zrok2(args[i])) continue;
        if((args[i+1]!="access"&&args[i+1]!="share")||args[i+2]!="private") return false;
        return !opal_endpoints_only||has_opal_endpoint(args);
    }
    return false;
}

bool pid_exists(pid_t pid) {
    return pid>0&&kill(pid,0)==0;
}

void terminate_pids(const std::vector<pid_t> &pids) {
    for(auto pid:pids) if(pid>0&&pid!=getpid()) kill(pid,SIGTERM);
    auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(1200);
    while(std::chrono::steady_clock::now()<deadline) {
        bool any=false;
        for(auto pid:pids) {
            if(pid<=0||pid==getpid()) continue;
            int status=0;
            pid_t rc=waitpid(pid,&status,WNOHANG);
            if(rc==pid) continue;
            if(pid_exists(pid)) any=true;
        }
        if(!any) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    for(auto pid:pids) if(pid>0&&pid!=getpid()&&pid_exists(pid)) kill(pid,SIGKILL);
    for(auto pid:pids) {
        if(pid<=0||pid==getpid()) continue;
        int status=0;
        waitpid(pid,&status,WNOHANG);
    }
}

std::vector<pid_t> scan_zrok_private_processes(bool opal_endpoints_only) {
    std::vector<pid_t> pids;
    std::error_code ec;
    for(const auto &entry:std::filesystem::directory_iterator("/proc",ec)) {
        if(ec) break;
        auto name=entry.path().filename().string();
        if(name.empty()||name.find_first_not_of("0123456789")!=std::string::npos) continue;
        pid_t pid=0;
        try { pid=static_cast<pid_t>(std::stol(name)); }
        catch(...) { continue; }
        if(pid!=getpid()&&is_zrok_private_process(pid,opal_endpoints_only)) pids.push_back(pid);
    }
    return pids;
}

std::filesystem::path pid_file(const char *kind) {
    return Paths::load().root/(std::string("tunnel-")+kind+".pids");
}

void record_pids(const std::filesystem::path &path,const std::vector<pid_t> &pids) {
    auto parent=path.parent_path();
    std::error_code ec;
    std::filesystem::create_directories(parent,ec);
    std::ofstream out(path,std::ios::trunc);
    for(auto pid:pids) {
        auto start=process_start_time(pid);
        if(pid>0&&start) out<<pid<<' '<<start<<'\n';
    }
}

void stop_recorded(const std::filesystem::path &path) {
    std::ifstream in(path);
    std::vector<pid_t> pids;
    pid_t pid=0;
    unsigned long long start=0;
    while(in>>pid>>start) {
        if(pid>0&&start&&process_start_time(pid)==start) pids.push_back(pid);
    }
    terminate_pids(pids);
    std::error_code ec;
    std::filesystem::remove(path,ec);
}

void stop_opal_tunnel_processes() {
    stop_recorded(pid_file("host"));
    stop_recorded(pid_file("access"));
    terminate_pids(scan_zrok_private_processes(true));
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
    stop_opal_tunnel_processes();
    auto control_pid=spawn({"zrok2","share","private","--headless","--share-token",control,"127.0.0.1:47990"},true);
    auto video_pid=spawn({"zrok2","share","private","--headless","--share-token",video,"127.0.0.1:47991"},true);
    bool control_started=child_started(control_pid);
    bool video_started=child_started(video_pid);
    if(!control_started||!video_started) {
        terminate_pids({control_pid,video_pid});
        if(debug_enabled()) std::cerr<<"OPAL zrok2 share missing or unavailable; repairing saved reservation(s)\n";
        if(!control_started) create_persistent_share(control);
        if(!video_started) create_persistent_share(video);
        control_pid=spawn({"zrok2","share","private","--headless","--share-token",control,"127.0.0.1:47990"},true);
        video_pid=spawn({"zrok2","share","private","--headless","--share-token",video,"127.0.0.1:47991"},true);
        control_started=child_started(control_pid);
        video_started=child_started(video_pid);
    }
    if(!control_started||!video_started) {
        terminate_pids({control_pid,video_pid});
        std::cerr<<"Could not start OPAL zrok2 tunnel\n";
        return 1;
    }
    record_pids(pid_file("host"),{control_pid,video_pid});
    return 0;
}

bool tunnel_access(const std::string &control_token,const std::string &video_token) {
    if(!ensure_zrok2()) return false;
    stop_opal_tunnel_processes();
    auto control_pid=spawn({"zrok2","access","private",control_token,"--bind","127.0.0.1:47990","--headless"},true);
    auto video_pid=spawn({"zrok2","access","private",video_token,"--bind","127.0.0.1:47991","--headless"},true);
    if(!wait_for_access_endpoints(control_pid,video_pid)) {
        terminate_pids({control_pid,video_pid});
        return false;
    }
    record_pids(pid_file("access"),{control_pid,video_pid});
    return true;
}

int tunnel_clean_local() {
    auto paths=Paths::load();
    Ini host;
    host.load(paths.host);
    auto control=host.get("tunnel","control_token");
    auto video=host.get("tunnel","video_token");

    stop_recorded(pid_file("host"));
    stop_recorded(pid_file("access"));
    terminate_pids(scan_zrok_private_processes(false));

    if(command_exists("zrok2")) {
        for(const auto &token:{control,video}) {
            if(token.empty()) continue;
            if(run_wait({"zrok2","delete","share",token})!=0)
                std::cerr<<"Warning: could not delete zrok share "<<token<<"\n";
        }
    }
    return 0;
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