#include <opal/tunnel.hpp>
#include <opal/config.hpp>
#include <opal/crypto.hpp>
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
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

namespace opal { namespace {

bool debug_enabled(){const char *value=std::getenv("OPAL_DEBUG");return value&&*value&&std::string(value)!="0";}

pid_t spawn(const std::vector<std::string> &args,bool quiet=false){
    pid_t pid=fork();if(pid==0){
        if(quiet&&!debug_enabled()){int nullfd=open("/dev/null",O_WRONLY|O_CLOEXEC);if(nullfd>=0){dup2(nullfd,STDOUT_FILENO);dup2(nullfd,STDERR_FILENO);if(nullfd>2)close(nullfd);}}
        std::vector<char*> argv;for(const auto &arg:args)argv.push_back(const_cast<char*>(arg.c_str()));argv.push_back(nullptr);execvp(argv[0],argv.data());_exit(127);
    }
    return pid;
}
int run_wait(const std::vector<std::string> &args){pid_t pid=spawn(args,true);if(pid<0)return 1;int status=0;if(waitpid(pid,&status,0)<0)return 1;return WIFEXITED(status)?WEXITSTATUS(status):1;}
int run_capture(const std::vector<std::string> &args,std::string &output){
    int fds[2];if(pipe(fds)!=0)return 1;pid_t pid=fork();if(pid==0){close(fds[0]);dup2(fds[1],STDOUT_FILENO);dup2(fds[1],STDERR_FILENO);close(fds[1]);std::vector<char*> argv;for(const auto &arg:args)argv.push_back(const_cast<char*>(arg.c_str()));argv.push_back(nullptr);execvp(argv[0],argv.data());_exit(127);}close(fds[1]);
    output.clear();char buffer[4096];for(;;){ssize_t n=read(fds[0],buffer,sizeof(buffer));if(n<=0)break;output.append(buffer,static_cast<std::size_t>(n));}close(fds[0]);if(pid<0)return 1;int status=0;if(waitpid(pid,&status,0)<0)return 1;return WIFEXITED(status)?WEXITSTATUS(status):1;
}
bool zrok2_environment_enabled(){std::string output;if(run_capture({"zrok2","status"},output)!=0){if(debug_enabled())std::cerr<<output;return false;}return output.find("EnvZId")!=std::string::npos;}
bool ensure_zrok2(){
    if(!command_exists("zrok2")){std::cerr<<"OPAL requires zrok2 for control networking. Install zrok2, then run opal again.\n";return false;}
    if(zrok2_environment_enabled())return true;
    std::cout<<"zrok2 is not enabled. Paste the token from `zrok enable <token>`: "<<std::flush;std::string token;if(!std::getline(std::cin,token))return false;token=trim(token);if(token.empty())return false;
    if(run_wait({"zrok2","enable",token})!=0||!zrok2_environment_enabled()){std::cerr<<"zrok2 enable failed\n";return false;}return true;
}
bool create_persistent_share(const std::string &token){return run_wait({"zrok2","create","share","--share-token",token,"--backend-mode","tcpTunnel"})==0;}
bool child_started(pid_t pid){if(pid<0)return false;std::this_thread::sleep_for(std::chrono::milliseconds(350));int status=0;return waitpid(pid,&status,WNOHANG)==0;}
bool child_alive(pid_t &pid){if(pid<0)return false;int status=0;pid_t rc=waitpid(pid,&status,WNOHANG);if(rc==0)return true;if(rc==pid)pid=-1;return false;}
bool local_port_ready(std::uint16_t port){int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return false;sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(port);inet_pton(AF_INET,"127.0.0.1",&addr.sin_addr);bool ok=connect(fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0;close(fd);return ok;}

unsigned long long process_start_time(pid_t pid){std::ifstream in("/proc/"+std::to_string(pid)+"/stat");std::string line;if(!std::getline(in,line))return 0;auto end=line.rfind(')');if(end==std::string::npos||end+2>=line.size())return 0;std::istringstream fields(line.substr(end+2));std::string value;for(int field=3;field<=22;++field){if(!(fields>>value))return 0;if(field==22)try{return std::stoull(value);}catch(...){return 0;}}return 0;}
std::vector<std::string> process_args(pid_t pid){std::ifstream in("/proc/"+std::to_string(pid)+"/cmdline",std::ios::binary);std::string raw((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());std::vector<std::string> args;std::size_t pos=0;while(pos<raw.size()){auto end=raw.find('\0',pos);if(end==std::string::npos)end=raw.size();if(end>pos)args.emplace_back(raw.substr(pos,end-pos));pos=end+1;}return args;}
bool basename_is_zrok2(const std::string &arg){return std::filesystem::path(arg).filename()=="zrok2";}
bool has_opal_endpoint(const std::vector<std::string> &args){for(const auto &arg:args)if(arg=="127.0.0.1:47990"||arg=="127.0.0.1:47991")return true;return false;}
bool is_zrok_private_process(pid_t pid,bool endpoints_only){auto args=process_args(pid);for(std::size_t i=0;i+2<args.size();++i){if(!basename_is_zrok2(args[i]))continue;if((args[i+1]!="access"&&args[i+1]!="share")||args[i+2]!="private")return false;return !endpoints_only||has_opal_endpoint(args);}return false;}
bool pid_exists(pid_t pid){return pid>0&&kill(pid,0)==0;}
bool process_active_or_reap(pid_t pid){
    if(pid<=0||pid==getpid())return false;
    int status=0;
    const pid_t rc=waitpid(pid,&status,WNOHANG);
    if(rc==pid)return false;
    if(rc<0&&errno!=ECHILD&&errno!=EINTR)return pid_exists(pid);
    return pid_exists(pid);
}
void terminate_pids(const std::vector<pid_t> &pids){
    for(auto pid:pids)if(pid>0&&pid!=getpid())kill(pid,SIGTERM);
    auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(1200);
    while(std::chrono::steady_clock::now()<deadline){
        bool any=false;
        for(auto pid:pids)if(process_active_or_reap(pid))any=true;
        if(!any)return;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    for(auto pid:pids)if(process_active_or_reap(pid))kill(pid,SIGKILL);
    deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(500);
    while(std::chrono::steady_clock::now()<deadline){
        bool any=false;
        for(auto pid:pids)if(process_active_or_reap(pid))any=true;
        if(!any)return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
std::vector<pid_t> scan_zrok_private_processes(bool endpoints_only){std::vector<pid_t> pids;std::error_code ec;for(const auto &entry:std::filesystem::directory_iterator("/proc",ec)){if(ec)break;auto name=entry.path().filename().string();if(name.empty()||name.find_first_not_of("0123456789")!=std::string::npos)continue;pid_t pid=0;try{pid=static_cast<pid_t>(std::stol(name));}catch(...){continue;}if(pid!=getpid()&&is_zrok_private_process(pid,endpoints_only))pids.push_back(pid);}return pids;}
std::filesystem::path pid_file(const char *kind){return Paths::load().root/(std::string("tunnel-")+kind+".pids");}
void record_pids(const std::filesystem::path &path,const std::vector<pid_t> &pids){std::error_code ec;std::filesystem::create_directories(path.parent_path(),ec);std::ofstream out(path,std::ios::trunc);for(auto pid:pids){auto start=process_start_time(pid);if(pid>0&&start)out<<pid<<' '<<start<<'\n';}}
bool recorded_processes_healthy(const std::filesystem::path &path,std::size_t expected){std::ifstream in(path);if(!in)return false;std::size_t count=0;pid_t pid=0;unsigned long long start=0;while(in>>pid>>start){++count;if(pid<=0||!start||process_start_time(pid)!=start||!pid_exists(pid)||!is_zrok_private_process(pid,true))return false;}return count==expected;}
void stop_recorded(const std::filesystem::path &path){std::ifstream in(path);std::vector<pid_t> pids;pid_t pid=0;unsigned long long start=0;while(in>>pid>>start)if(pid>0&&start&&process_start_time(pid)==start)pids.push_back(pid);terminate_pids(pids);std::error_code ec;std::filesystem::remove(path,ec);}
void stop_opal_tunnel_processes(){stop_recorded(pid_file("host"));stop_recorded(pid_file("access"));terminate_pids(scan_zrok_private_processes(true));}
std::string make_token(){return std::string("opal-ctl-")+random_hex(5);}

bool share_present(const std::string &token,bool &known){std::string output;known=run_capture({"zrok2","list","shares"},output)==0;if(!known)return false;return output.find(token)!=std::string::npos;}
bool retire_legacy_video_share(const std::string &token){
    if(token.empty())return true;bool known=false;bool present=share_present(token,known);if(!known)return false;if(present)run_wait({"zrok2","delete","share",token});
    for(int attempt=0;attempt<5;++attempt){present=share_present(token,known);if(known&&!present)return true;std::this_thread::sleep_for(std::chrono::milliseconds(100));}return false;
}
}

bool tunnel_connection_code(const std::string &code,std::string *control_token,std::string *legacy_video_token){
    if(code.rfind("opal:",0)!=0)return false;auto body=code.substr(5);if(body.empty())return false;auto comma=body.find(',');std::string control,legacy;
    if(comma==std::string::npos)control=body;else{if(comma==0||comma+1>=body.size()||body.find(',',comma+1)!=std::string::npos)return false;control=body.substr(0,comma);legacy=body.substr(comma+1);}
    if(control.empty())return false;if(control_token)*control_token=control;if(legacy_video_token)*legacy_video_token=legacy;return true;
}

int tunnel_host_setup(std::string &connection_code){
    if(!ensure_zrok2())return 2;auto paths=Paths::load();ensure_layout(paths);Ini host;host.load(paths.host);auto control=host.get("tunnel","control_token");
    if(control.empty())for(int attempt=0;attempt<4&&control.empty();++attempt){auto candidate=make_token();if(create_persistent_share(candidate))control=candidate;}
    if(control.empty()){std::cerr<<"Could not allocate persistent zrok2 control share\n";return 1;}
    auto legacy=host.get("tunnel","video_token");if(!legacy.empty()){if(retire_legacy_video_share(legacy))host.set("tunnel","video_token","");else std::cerr<<"Warning: could not confirm deletion of legacy OPAL video share "<<legacy<<"\n";}
    host.set("tunnel","control_token",control);host.set("tunnel","mode","zrok2-control-only");if(!host.save(paths.host))return 1;connection_code="opal:"+control;return 0;
}

int tunnel_host_start(){
    std::string code;int setup=tunnel_host_setup(code);if(setup!=0)return setup;auto paths=Paths::load();Ini host;host.load(paths.host);auto control=host.get("tunnel","control_token");stop_opal_tunnel_processes();
    auto pid=spawn({"zrok2","share","private","--headless","--share-token",control,"127.0.0.1:47990"},true);if(!child_started(pid)){terminate_pids({pid});create_persistent_share(control);pid=spawn({"zrok2","share","private","--headless","--share-token",control,"127.0.0.1:47990"},true);}
    if(!child_started(pid)){terminate_pids({pid});std::cerr<<"Could not start OPAL zrok2 control tunnel\n";return 1;}record_pids(pid_file("host"),{pid});return 0;
}
bool tunnel_host_healthy(){return recorded_processes_healthy(pid_file("host"),1);}
int tunnel_host_ensure_running(){return tunnel_host_healthy()?0:tunnel_host_start();}

bool tunnel_access(const std::string &control_token,int timeout_ms){
    if(!ensure_zrok2()||control_token.empty())return false;stop_opal_tunnel_processes();auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(std::max(1,timeout_ms));int retry=1000;
    while(std::chrono::steady_clock::now()<deadline){auto pid=spawn({"zrok2","access","private",control_token,"--bind","127.0.0.1:47990","--headless","--force-local"},true);while(std::chrono::steady_clock::now()<deadline){if(!child_alive(pid))break;if(local_port_ready(47990)){record_pids(pid_file("access"),{pid});return true;}std::this_thread::sleep_for(std::chrono::milliseconds(100));}terminate_pids({pid});auto now=std::chrono::steady_clock::now();if(now>=deadline)break;auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count();auto pause=std::min<long long>(retry,remaining);if(pause>0)std::this_thread::sleep_for(std::chrono::milliseconds(pause));retry=std::min(5000,retry*2);}return false;
}

int tunnel_clean_local(){
    auto paths=Paths::load();Ini host;host.load(paths.host);auto control=host.get("tunnel","control_token"),legacy=host.get("tunnel","video_token");stop_recorded(pid_file("host"));stop_recorded(pid_file("access"));terminate_pids(scan_zrok_private_processes(false));
    if(command_exists("zrok2"))for(const auto &token:{control,legacy})if(!token.empty()&&run_wait({"zrok2","delete","share",token})!=0)std::cerr<<"Warning: could not delete zrok share "<<token<<"\n";return 0;
}

int tunnel_host(){std::string code;int rc=tunnel_host_setup(code);if(rc!=0)return rc;std::cout<<"OPAL connection code: "<<code<<"\n";rc=tunnel_host_start();if(rc!=0)return rc;std::cout<<"Control tunnel running. Keep this process alive.\n";for(;;)pause();}

}
