#include <opal/tunnel.hpp>
#include <opal/config.hpp>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include <signal.h>
namespace opal {
static std::string zrok_bin(){if(command_exists("zrok2"))return"zrok2";if(command_exists("zrok"))return"zrok";return{};}
static pid_t spawn(const std::vector<std::string>&a){pid_t p=fork();if(p==0){std::vector<char*>v;for(auto&s:a)v.push_back(const_cast<char*>(s.c_str()));v.push_back(nullptr);execvp(v[0],v.data());_exit(127);}return p;}
int tunnel_host(){auto z=zrok_bin();if(z.empty()){std::cerr<<"zrok/zrok2 not installed\n";return 2;}std::cout<<"Starting private OPAL control and video shares. Copy both share tokens.\n";auto a=spawn({z,"share","private","--backend-mode","tcpTunnel","127.0.0.1:47990"});auto b=spawn({z,"share","private","--backend-mode","tcpTunnel","127.0.0.1:47991"});int st=0;waitpid(a,&st,0);kill(b,SIGTERM);waitpid(b,nullptr,0);return WIFEXITED(st)?WEXITSTATUS(st):1;}
bool tunnel_access(const std::string&ct,const std::string&vt){auto z=zrok_bin();if(z.empty())return false;spawn({z,"access","private",ct,"--bind","127.0.0.1:47990","--headless"});spawn({z,"access","private",vt,"--bind","127.0.0.1:47991","--headless"});std::this_thread::sleep_for(std::chrono::seconds(2));return true;}
}
