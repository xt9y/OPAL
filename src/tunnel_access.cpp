#include <opal/tunnel_access.hpp>
#include <opal/config.hpp>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <iostream>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace opal { namespace {
void cloexec(int fd){int f=fcntl(fd,F_GETFD,0);if(f>=0)fcntl(fd,F_SETFD,f|FD_CLOEXEC);}
pid_t spawn(const std::vector<std::string>&args){pid_t pid=fork();if(pid==0){prctl(PR_SET_PDEATHSIG,SIGTERM);if(getppid()==1)_exit(1);int n=open("/dev/null",O_WRONLY|O_CLOEXEC);if(n>=0){dup2(n,STDOUT_FILENO);dup2(n,STDERR_FILENO);if(n>2)close(n);}std::vector<char*>v;for(auto&a:args)v.push_back(const_cast<char*>(a.c_str()));v.push_back(nullptr);execvp(v[0],v.data());_exit(127);}return pid;}
bool alive(pid_t&pid){if(pid<=0)return false;int st=0;pid_t rc=waitpid(pid,&st,WNOHANG);if(rc==0)return true;if(rc==pid)pid=-1;return false;}
void stop(pid_t&pid){if(pid<=0)return;kill(pid,SIGTERM);auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(800);while(std::chrono::steady_clock::now()<end){int st=0;pid_t rc=waitpid(pid,&st,WNOHANG);if(rc==pid){pid=-1;return;}std::this_thread::sleep_for(std::chrono::milliseconds(25));}kill(pid,SIGKILL);int st=0;while(waitpid(pid,&st,0)<0&&errno==EINTR){}pid=-1;}
int free_port(){int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return 0;cloexec(fd);sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=0;if(bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))!=0){close(fd);return 0;}socklen_t n=sizeof(a);if(getsockname(fd,reinterpret_cast<sockaddr*>(&a),&n)!=0){close(fd);return 0;}int p=ntohs(a.sin_port);close(fd);return p;}
bool ready(int port){int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return false;cloexec(fd);sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(static_cast<uint16_t>(port));inet_pton(AF_INET,"127.0.0.1",&a.sin_addr);bool ok=connect(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0;close(fd);return ok;}
bool zrok_enabled(){if(!command_exists("zrok2")){std::cerr<<"OPAL requires zrok2 for networking.\n";return false;}int p[2];if(pipe(p)!=0)return false;cloexec(p[0]);cloexec(p[1]);pid_t pid=fork();if(pid==0){dup2(p[1],STDOUT_FILENO);dup2(p[1],STDERR_FILENO);close(p[0]);close(p[1]);execlp("zrok2","zrok2","status",static_cast<char*>(nullptr));_exit(127);}close(p[1]);std::string out;char b[1024];ssize_t n;while((n=read(p[0],b,sizeof(b)))>0)out.append(b,n);close(p[0]);int st=0;waitpid(pid,&st,0);if(WIFEXITED(st)&&WEXITSTATUS(st)==0&&out.find("EnvZId")!=std::string::npos)return true;std::cout<<"zrok2 is not enabled. Paste the token from `zrok enable <token>`: "<<std::flush;std::string token;if(!std::getline(std::cin,token)||trim(token).empty())return false;token=trim(token);pid=fork();if(pid==0){execlp("zrok2","zrok2","enable",token.c_str(),static_cast<char*>(nullptr));_exit(127);}waitpid(pid,&st,0);return WIFEXITED(st)&&WEXITSTATUS(st)==0;}
} 

void tunnel_access_stop(TunnelAccessHandle&h){stop(h.control_pid);stop(h.video_pid);h.control_port=0;h.video_port=0;}
bool tunnel_access_healthy(TunnelAccessHandle&h){return alive(h.control_pid)&&alive(h.video_pid)&&h.control_port>0&&h.video_port>0&&ready(h.control_port)&&ready(h.video_port);}
bool tunnel_access_start(TunnelAccessHandle&h,const std::string&ct,const std::string&vt,int timeout_ms){if(tunnel_access_healthy(h))return true;tunnel_access_stop(h);if(ct.empty()||vt.empty()||!zrok_enabled())return false;auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(std::max(1,timeout_ms));while(std::chrono::steady_clock::now()<deadline){h.control_port=free_port();h.video_port=free_port();if(h.control_port<=0||h.video_port<=0||h.control_port==h.video_port){tunnel_access_stop(h);continue;}h.control_pid=spawn({"zrok2","access","private",ct,"--bind","127.0.0.1:"+std::to_string(h.control_port),"--headless","--force-local"});h.video_pid=spawn({"zrok2","access","private",vt,"--bind","127.0.0.1:"+std::to_string(h.video_port),"--headless","--force-local"});while(std::chrono::steady_clock::now()<deadline){if(!alive(h.control_pid)||!alive(h.video_pid))break;if(ready(h.control_port)&&ready(h.video_port))return true;std::this_thread::sleep_for(std::chrono::milliseconds(100));}tunnel_access_stop(h);std::this_thread::sleep_for(std::chrono::milliseconds(150));}return false;}
}
