#include <opal/setup.hpp>
#include <opal/config.hpp>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

namespace opal {
int init_calls=0,host_setup_calls=0,host_run_calls=0,host_service_calls=0,client_calls=0,wake_calls=0,add_calls=0,tunnel_setup_calls=0,tunnel_start_calls=0;
std::string client_target,added_address;

int init(){++init_calls;ensure_layout(Paths::load());return 0;}
int doctor(){return 0;}
int host_setup(){++host_setup_calls;ensure_layout(Paths::load());Ini h;h.set("host","password","TEST");h.save(Paths::load().host);return 0;}
int host_run(){++host_run_calls;return 0;}
int host_service(bool){++host_service_calls;return 0;}
int bridge_setup(const char*){return 0;}
int hosts_list(){return 0;}
int hosts_add(const std::string&name,const std::string&address,const std::string&mac){++add_calls;added_address=address;Ini h;h.load(Paths::load().hosts);h.set(name,"address",address);if(!mac.empty())h.set(name,"mac",mac);h.save(Paths::load().hosts);return 0;}
int client_connect(const std::string&target,const std::string&){++client_calls;client_target=target;return 0;}
int wake_named(const std::string&){++wake_calls;return 0;}
int tunnel_host_setup(std::string &code){++tunnel_setup_calls;code="opal:control-token,video-token";return 0;}
int tunnel_host_start(){++tunnel_start_calls;return 0;}
bool tunnel_connection_code(const std::string&code,std::string*control,std::string*video){if(code!="opal:control-token,video-token")return false;if(control)*control="control-token";if(video)*video="video-token";return true;}
}

static void reset_calls(){
    opal::init_calls=opal::host_setup_calls=opal::host_run_calls=opal::host_service_calls=opal::client_calls=opal::wake_calls=opal::add_calls=opal::tunnel_setup_calls=opal::tunnel_start_calls=0;
    opal::client_target.clear();opal::added_address.clear();
}

static std::filesystem::path fresh(const char*name){
    auto p=std::filesystem::temp_directory_path()/name;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    setenv("OPAL_HOME",p.c_str(),1);
    return p;
}

static int run_with(const std::string&input,std::string*out=nullptr){
    std::istringstream in(input);std::ostringstream capture;
    auto*oldin=std::cin.rdbuf(in.rdbuf());auto*oldout=std::cout.rdbuf(capture.rdbuf());
    int rc=opal::interactive_run();
    std::cin.rdbuf(oldin);std::cout.rdbuf(oldout);if(out)*out=capture.str();return rc;
}

int main(){
    {
        auto p=fresh("opal-setup-host-test");reset_calls();std::string out;
        assert(run_with("1\ny\nn\n",&out)==0);
        opal::Ini c;assert(c.load(p/"config.ini"));assert(c.get("opal","role")=="host");
        assert(opal::host_setup_calls==1);assert(opal::tunnel_setup_calls==1);assert(opal::tunnel_start_calls==1);assert(opal::host_run_calls==1);
        assert(out.find("opal:control-token,video-token")!=std::string::npos);
    }
    {
        auto p=fresh("opal-setup-client-test");reset_calls();
        assert(run_with("2\nopal:control-token,video-token\ndesktop\n")==0);
        opal::Ini c;assert(c.load(p/"config.ini"));assert(c.get("opal","role")=="client");assert(c.get("opal","default_host")=="desktop");
        assert(opal::add_calls==1);assert(opal::added_address=="opal:control-token,video-token");assert(opal::client_calls==1);assert(opal::client_target=="desktop");
    }
    {
        auto p=fresh("opal-auto-host-test");reset_calls();opal::Ini c;c.set("opal","role","host");c.save(p/"config.ini");
        assert(run_with("")==0);assert(opal::tunnel_start_calls==1);assert(opal::host_run_calls==1);
    }
    {
        auto p=fresh("opal-auto-client-test");reset_calls();opal::Ini c;c.set("opal","role","client");c.set("opal","default_host","desktop");c.save(p/"config.ini");opal::Ini h;h.set("desktop","address","opal:control-token,video-token");h.save(p/"hosts.ini");
        assert(run_with("")==0);assert(opal::wake_calls==0);assert(opal::client_calls==1);assert(opal::client_target=="desktop");
    }
    std::cout<<"setup tests passed\n";
}
