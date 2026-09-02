#include <opal/setup.hpp>
#include <opal/client.hpp>
#include <opal/config.hpp>
#include <opal/host.hpp>
#include <opal/system.hpp>
#include <opal/tunnel.hpp>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace opal {
namespace {
std::string read_line(const char *prompt,const std::string &fallback={}) {
    std::cout<<prompt;
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
    auto p=Paths::load();ensure_layout(p);Ini cfg;cfg.load(p.config);
    cfg.set("opal","role",role);
    if(!default_host.empty()) cfg.set("opal","default_host",default_host);
    return cfg.save(p.config);
}

std::string detect_mac() {
    std::error_code ec;
    for(const auto &entry:std::filesystem::directory_iterator("/sys/class/net",ec)) {
        if(ec) break;
        auto name=entry.path().filename().string();if(name=="lo") continue;
        std::ifstream state(entry.path()/"operstate");std::string s;state>>s;
        if(s!="up"&&s!="unknown") continue;
        std::ifstream mac(entry.path()/"address");std::string m;mac>>m;
        if(m.size()==17) return m;
    }
    return {};
}

void configure_host_wol() {
    auto p=Paths::load();Ini host;host.load(p.host);host.set("host","wol","true");
    auto mac=detect_mac();if(!mac.empty()) host.set("host","mac",mac);host.save(p.host);
    std::cout<<"Wake-on-LAN enabled in OPAL";
    if(!mac.empty()) std::cout<<" (MAC "<<mac<<")";
    std::cout<<". Ensure WoL is enabled in firmware/NIC settings.\n";
}

int connect_default(const Ini &cfg) {
    auto name=cfg.get("opal","default_host");
    if(name.empty()) {std::cerr<<"No default host configured. Run 'opal setup'.\n";return 2;}
    std::cout<<"Connecting to "<<name<<" through OPAL tunnel...\n";
    return client_connect(name);
}

int ensure_host_service() {
    if(host_service(true)!=0) {
        std::cerr<<"Could not start OPAL host service. Run 'systemctl --user status opal-host.service'.\n";
        return 1;
    }
    std::cout<<"OPAL host service running.\n";
    return 0;
}

int first_setup() {
    std::cout<<"OPAL SETUP\n--------------------------------\n1  Host this computer\n2  Connect to another computer\n3  Quit\n> ";
    std::string choice;if(!std::getline(std::cin,choice)) return 0;choice=trim(choice);
    if(choice=="1") {
        if(init()!=0||host_setup()!=0) return 1;
        std::string code;
        if(tunnel_host_setup(code)!=0) return 1;
        if(!save_role("host")) return 1;
        if(ask_yes_no("Enable Wake-on-LAN support? [Y/n] ",true)) configure_host_wol();
        std::cout<<"\nOPAL connection code\n"<<code<<"\n\nGive this code to the client. No IP address or port forwarding is required.\n";
        return ensure_host_service();
    }
    if(choice=="2") {
        if(init()!=0) return 1;
        auto code=read_line("OPAL connection code: ");
        std::string control,video;
        if(!tunnel_connection_code(code,&control,&video)) {std::cerr<<"Invalid OPAL connection code\n";return 2;}
        auto name=read_line("Save as [desktop]: ","desktop");
        if(hosts_add(name,code)!=0||!save_role("client",name)) return 1;
        return client_connect(name);
    }
    return 0;
}
}

int interactive_setup(){return first_setup();}

int interactive_run() {
    auto p=Paths::load();Ini cfg;
    if(!cfg.load(p.config)||cfg.get("opal","role").empty()) return first_setup();
    auto role=cfg.get("opal","role");
    if(role=="host") return ensure_host_service();
    if(role=="client") return connect_default(cfg);
    return first_setup();
}
}
