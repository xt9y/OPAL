#include <opal/setup.hpp>
#include <opal/client.hpp>
#include <opal/config.hpp>
#include <opal/host.hpp>
#include <opal/system.hpp>
#include <opal/wake.hpp>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace opal {
namespace {
std::string read_line(const char *prompt,const std::string &fallback={}) {
    std::cout << prompt;
    std::string value;
    if(!std::getline(std::cin,value)) return fallback;
    value=trim(value);
    return value.empty()?fallback:value;
}

bool ask_yes_no(const char *prompt,bool fallback) {
    auto value=read_line(prompt);
    if(value.empty()) return fallback;
    char c=static_cast<char>(std::tolower(static_cast<unsigned char>(value[0])));
    return c=='y'||c=='1';
}

bool save_role(const std::string &role,const std::string &default_host={}) {
    auto p=Paths::load();
    ensure_layout(p);
    Ini cfg;
    cfg.load(p.config);
    cfg.set("opal","role",role);
    if(!default_host.empty()) cfg.set("opal","default_host",default_host);
    return cfg.save(p.config);
}

std::string detect_mac() {
    std::error_code ec;
    for(const auto &entry:std::filesystem::directory_iterator("/sys/class/net",ec)) {
        if(ec) break;
        auto name=entry.path().filename().string();
        if(name=="lo") continue;
        std::ifstream state(entry.path()/"operstate");
        std::string s;
        state>>s;
        if(s!="up"&&s!="unknown") continue;
        std::ifstream mac(entry.path()/"address");
        std::string m;
        mac>>m;
        if(m.size()==17) return m;
    }
    return {};
}

void configure_host_wol() {
    auto p=Paths::load();
    Ini host;
    host.load(p.host);
    host.set("host","wol","true");
    auto mac=detect_mac();
    if(!mac.empty()) host.set("host","mac",mac);
    host.save(p.host);
    std::cout<<"Wake-on-LAN enabled in OPAL";
    if(!mac.empty()) std::cout<<" (MAC "<<mac<<")";
    std::cout<<". Ensure WoL is enabled in firmware/NIC settings.\n";
}

bool reachable(const std::string &host,int port,int timeout_ms=300) {
    if(host.empty()||host.rfind("zrok:",0)==0) return false;
    addrinfo hints{};
    hints.ai_socktype=SOCK_STREAM;
    hints.ai_family=AF_UNSPEC;
    addrinfo *list=nullptr;
    auto service=std::to_string(port);
    if(getaddrinfo(host.c_str(),service.c_str(),&hints,&list)!=0) return false;
    bool ok=false;
    for(auto *ai=list;ai&&!ok;ai=ai->ai_next) {
        int fd=socket(ai->ai_family,ai->ai_socktype,ai->ai_protocol);
        if(fd<0) continue;
        int flags=fcntl(fd,F_GETFL,0);
        if(flags>=0) fcntl(fd,F_SETFL,flags|O_NONBLOCK);
        int rc=connect(fd,ai->ai_addr,ai->ai_addrlen);
        if(rc==0) ok=true;
        else if(errno==EINPROGRESS) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd,&wfds);
            timeval tv{timeout_ms/1000,(timeout_ms%1000)*1000};
            rc=select(fd+1,nullptr,&wfds,nullptr,&tv);
            if(rc>0&&FD_ISSET(fd,&wfds)) {
                int err=0;
                socklen_t len=sizeof(err);
                if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&err,&len)==0&&err==0) ok=true;
            }
        }
        close(fd);
    }
    freeaddrinfo(list);
    return ok;
}

int connect_default(const Ini &cfg) {
    auto p=Paths::load();
    auto name=cfg.get("opal","default_host");
    if(name.empty()) {
        std::cerr<<"No default host configured. Run 'opal setup'.\n";
        return 2;
    }
    Ini hosts;
    if(!hosts.load(p.hosts)) {
        std::cerr<<"Saved host not found. Run 'opal setup'.\n";
        return 2;
    }
    auto address=hosts.get(name,"address");
    int port=hosts.get_int(name,"port",47990);
    auto mac=hosts.get(name,"mac");
    if(!reachable(address,port)&&!mac.empty()) {
        std::cout<<"Host "<<name<<" is offline. Waking it...\n";
        if(wake_named(name)==0) {
            int seconds=cfg.get_int("network","connect_wait_seconds",45);
            for(int i=0;i<seconds*2&&!reachable(address,port);++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
    std::cout<<"Connecting to "<<name<<"...\n";
    return client_connect(name);
}

int first_setup() {
    std::cout<<"OPAL SETUP\n--------------------------------\n1  Host this computer\n2  Connect to another computer\n3  Quit\n> ";
    std::string choice;
    if(!std::getline(std::cin,choice)) return 0;
    choice=trim(choice);
    if(choice=="1") {
        if(init()!=0||host_setup()!=0) return 1;
        if(!save_role("host")) return 1;
        if(ask_yes_no("Enable Wake-on-LAN support? [Y/n] ",true)) configure_host_wol();
        if(ask_yes_no("Start OPAL automatically with this user session? [y/N] ",false)) {
            if(host_service(true)==0) {
                std::cout<<"OPAL host service started.\n";
                return 0;
            }
            std::cerr<<"Could not enable the user service; hosting will start in this terminal.\n";
        }
        return host_run();
    }
    if(choice=="2") {
        if(init()!=0) return 1;
        auto address=read_line("Host/IP: ");
        if(address.empty()) return 2;
        auto name=read_line("Save as [desktop]: ","desktop");
        auto mac=read_line("Wake-on-LAN MAC (optional): ");
        if(hosts_add(name,address,mac)!=0||!save_role("client",name)) return 1;
        return client_connect(name);
    }
    return 0;
}
}

int interactive_setup() { return first_setup(); }

int interactive_run() {
    auto p=Paths::load();
    Ini cfg;
    if(!cfg.load(p.config)||cfg.get("opal","role").empty()) return first_setup();
    auto role=cfg.get("opal","role");
    if(role=="host") return host_run();
    if(role=="client") return connect_default(cfg);
    return first_setup();
}
}
