#include <opal/setup.hpp>
#include <opal/client.hpp>
#include <opal/config.hpp>
#include <opal/connection_code.hpp>
#include <opal/crypto.hpp>
#include <opal/host.hpp>
#include <opal/pipewire_capture.hpp>
#include <opal/platform.hpp>
#include <opal/system.hpp>
#include <opal/tailnet.hpp>
#include <opal/wake.hpp>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace opal {
namespace {
std::string read_line(const char *prompt,const std::string &fallback={}){std::cout<<prompt;std::string value;if(!std::getline(std::cin,value))return fallback;value=trim(value);return value.empty()?fallback:value;}
bool ask_yes_no(const char *prompt,bool fallback){auto value=read_line(prompt);if(value.empty())return fallback;char c=static_cast<char>(std::tolower(static_cast<unsigned char>(value[0])));return c=='y'||c=='1';}
bool save_role(const std::string &role,const std::string &default_host={}){auto p=Paths::load();ensure_layout(p);Ini cfg;cfg.load(p.config);cfg.set("opal","role",role);if(!default_host.empty())cfg.set("opal","default_host",default_host);return cfg.save(p.config);}
std::vector<std::string> host_names(const Ini &hosts){std::vector<std::string>names;for(const auto &[name,values]:hosts.sections()){(void)values;if(!name.empty())names.push_back(name);}return names;}
std::string choose_host(const Ini &hosts,const char *title){auto names=host_names(hosts);if(names.empty()){std::cout<<"No saved hosts.\n";return{};}std::cout<<title<<"\n--------------------------------\n";for(size_t i=0;i<names.size();++i)std::cout<<(i+1)<<"  "<<names[i]<<"\n";std::cout<<"> ";std::string choice;if(!std::getline(std::cin,choice))return{};choice=trim(choice);try{size_t used=0;unsigned long index=std::stoul(choice,&used);if(used!=choice.size()||index<1||index>names.size())return{};return names[index-1];}catch(...){return{};}}
std::string saved_connection_id(const Ini&hosts,const std::string&name){auto id=hosts.get(name,"connection_id");if(id.empty())id=hosts.get(name,"rendezvous_id");return id;}
void print_local_host_credentials(const Paths &p){Ini host;if(!host.load(p.host))return;auto password=normalize_pairing_code(host.get("host","password"));if(password.empty()||!std::filesystem::exists(p.identity_pub))return;auto code=format_connection_code(connection_id_from_public_key(public_key_hex(p.identity_pub)));if(code.empty())return;std::cout<<"Local host\nConnection code: "<<code<<"\nPairing password: "<<password<<"\n";}
bool wayland_session(){const char*display=std::getenv("WAYLAND_DISPLAY");return display&&*display;}
std::string detect_mac(){std::error_code ec;for(const auto &entry:std::filesystem::directory_iterator("/sys/class/net",ec)){if(ec)break;auto name=entry.path().filename().string();if(name=="lo")continue;std::ifstream state(entry.path()/"operstate");std::string s;state>>s;if(s!="up"&&s!="unknown")continue;std::ifstream mac(entry.path()/"address");std::string m;mac>>m;if(m.size()==17)return m;}return{};}
void configure_host_wol(){auto p=Paths::load();Ini host;host.load(p.host);host.set("host","wol","true");auto mac=detect_mac();if(!mac.empty())host.set("host","mac",mac);host.save(p.host);std::cout<<"Wake-on-LAN enabled in OPAL";if(!mac.empty())std::cout<<" (MAC "<<mac<<")";std::cout<<". Ensure WoL is enabled in firmware/NIC settings.\n";}
bool configure_host_screens(bool remember){auto p=Paths::load();Ini host;host.load(p.host);host.set("host","remember_screens",remember?"true":"false");if(!host.save(p.host))return false;if(!remember)return true;StreamOptions capture{7680,4320,60};std::string error;const auto token=(p.root/"portal-session.token").string();std::cout<<"Select every monitor OPAL may share. This authorization will be reused automatically.\n";if(!native_pipewire_prepare(capture,token,&error)){std::cerr<<"Could not authorize persistent Linux screen capture"<<(error.empty()?"":": "+error)<<"\n";return false;}std::cout<<"OPAL screen selection saved. Normal reconnect/recovery will reuse it without reopening the chooser.\n";return true;}
int connect_default(const Ini &cfg,const StreamOptions &stream){auto name=cfg.get("opal","default_host");if(name.empty())return-1;auto p=Paths::load();Ini hosts;hosts.load(p.hosts);if(!hosts.get(name,"mac").empty()){std::cout<<"Waking "<<name<<"...\n";(void)wake_named(name);}std::cout<<"Connecting to "<<name<<"...\n";return client_connect(name,"",stream);}
int ensure_host_service(){if(host_service(true)!=0){std::cerr<<"Could not start OPAL host service. Run 'opal doctor' for platform diagnostics.\n";return 1;}std::cout<<"OPAL host service running.\n";return 0;}

int first_setup(const StreamOptions &stream={}){
    if(require_tailscale()!=0)return 1;
    std::cout<<"OPAL SETUP\n--------------------------------\n1  Host this computer\n2  Connect to another computer\n3  Quit\n> ";std::string choice;if(!std::getline(std::cin,choice))return 0;choice=trim(choice);
    if(choice=="1"){
        if(init()!=0||host_setup()!=0)return 1;
        if(!save_role("host"))return 1;
        if(current_platform()==PlatformKind::Linux){
            if(wayland_session()){
                const bool remember=ask_yes_no("Remember selected screens for automatic hosting? [Y/n] ",true);
                if(!configure_host_screens(remember))return 1;
            }
            if(ask_yes_no("Enable Wake-on-LAN support? [Y/n] ",true))configure_host_wol();
        }
        return ensure_host_service();
    }
    if(choice=="2"){if(init()!=0)return 1;auto code=read_line("OPAL connection code: ");std::string id;if(!parse_connection_code(code,id)){std::cerr<<"Invalid OPAL connection code. Expected: XXXX-XXXX-XXXX\n";return 2;}auto name=read_line("Save as [desktop]: ","desktop");if(hosts_add(name,code)!=0||!save_role("client",name))return 1;return client_connect(name,"",stream);}return 0;
}
}

int interactive_setup(){return first_setup();}
int interactive_select(){auto p=Paths::load();Ini hosts;hosts.load(p.hosts);auto names=host_names(hosts);auto name=choose_host(hosts,"SELECT HOST");if(name.empty()){if(names.empty())print_local_host_credentials(p);return names.empty()?0:2;}ensure_layout(p);Ini cfg;cfg.load(p.config);cfg.set("opal","role","client");cfg.set("opal","default_host",name);if(!cfg.save(p.config))return 1;std::cout<<"Selected "<<name<<".\n";auto id=saved_connection_id(hosts,name);auto code=id.empty()?hosts.get(name,"connection_code"):format_connection_code(id);if(!code.empty())std::cout<<"Connection code: "<<code<<"\n";print_local_host_credentials(p);return 0;}
int interactive_remove(){auto p=Paths::load();Ini hosts;hosts.load(p.hosts);auto selected=choose_host(hosts,"REMOVE HOST");auto names=host_names(hosts);if(selected.empty())return names.empty()?0:2;Ini remaining;for(const auto &[name,values]:hosts.sections()){if(name.empty()||name==selected)continue;for(const auto &[key,value]:values)remaining.set(name,key,value);}if(!remaining.save(p.hosts))return 1;Ini cfg;cfg.load(p.config);if(cfg.get("opal","default_host")==selected){auto left=host_names(remaining);cfg.set("opal","default_host",left.empty()?"":left.front());if(!cfg.save(p.config))return 1;}std::cout<<"Removed "<<selected<<".\n";return 0;}
int interactive_restart(){auto p=Paths::load();Ini cfg;cfg.load(p.config);auto role=cfg.get("opal","role");if(role.empty()){Ini hosts;hosts.load(p.hosts);auto names=host_names(hosts);const bool local_host=std::filesystem::exists(p.host);if(names.size()==1&&!local_host){if(!save_role("client",names.front()))return 1;cfg.load(p.config);return connect_default(cfg,{});}if(names.empty()&&local_host){if(!save_role("host"))return 1;if(require_tailscale()!=0)return 1;return restart_services();}return first_setup();}if(role=="client")return interactive_run();if(role=="host"){if(require_tailscale()!=0)return 1;return restart_services();}return first_setup();}
int interactive_run(const StreamOptions &stream){auto p=Paths::load();Ini cfg;if(!cfg.load(p.config)||cfg.get("opal","role").empty())return first_setup(stream);auto role=cfg.get("opal","role");if(role=="host"){if(require_tailscale()!=0)return 1;return ensure_host_service();}if(role=="client"){int rc=connect_default(cfg,stream);if(rc==-1)return first_setup(stream);return rc;}return first_setup(stream);}
}
