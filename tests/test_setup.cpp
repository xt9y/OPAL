#include <opal/setup.hpp>
#include <opal/config.hpp>
#include <opal/media_profile.hpp>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

namespace opal {
int interactive_select();
int interactive_remove();
int init_calls=0,host_setup_calls=0,host_run_calls=0,host_service_calls=0,client_calls=0,wake_calls=0,add_calls=0,tunnel_setup_calls=0,tunnel_start_calls=0;
std::string client_target,added_address,wake_target;
StreamOptions client_stream;

int init(){++init_calls;ensure_layout(Paths::load());return 0;}
int doctor(){return 0;}
int host_setup(){++host_setup_calls;ensure_layout(Paths::load());Ini h;h.set("host","password","TEST");h.save(Paths::load().host);return 0;}
int host_run(){++host_run_calls;return 0;}
int host_service(bool){++host_service_calls;return 0;}
int bridge_setup(const char*){return 0;}
int hosts_list(){return 0;}
int hosts_add(const std::string&name,const std::string&address,const std::string&mac){++add_calls;added_address=address;Ini h;h.load(Paths::load().hosts);h.set(name,"address",address);if(!mac.empty())h.set(name,"mac",mac);h.save(Paths::load().hosts);return 0;}
int client_connect(const std::string&target,const std::string&password,const StreamOptions&stream){(void)password;++client_calls;client_target=target;client_stream=stream;return 0;}
int client_connect(const std::string&target,const std::string&password){return client_connect(target,password,{});}
int wake_named(const std::string&name){++wake_calls;wake_target=name;return 0;}
int tunnel_host_setup(std::string &code){++tunnel_setup_calls;code="opal:control-token";return 0;}
int tunnel_host_start(){++tunnel_start_calls;return 0;}
bool tunnel_connection_code(const std::string&code,std::string*control,std::string*legacy){
    if(code=="opal:control-token"){if(control)*control="control-token";if(legacy)*legacy="";return true;}
    if(code=="opal:control-token,legacy-video-token"){if(control)*control="control-token";if(legacy)*legacy="legacy-video-token";return true;}
    return false;
}
}

static void reset_calls(){
    opal::init_calls=opal::host_setup_calls=opal::host_run_calls=opal::host_service_calls=opal::client_calls=opal::wake_calls=opal::add_calls=opal::tunnel_setup_calls=opal::tunnel_start_calls=0;
    opal::client_target.clear();opal::added_address.clear();opal::wake_target.clear();opal::client_stream={};
}

static std::filesystem::path fresh(const char*name){
    auto p=std::filesystem::temp_directory_path()/name;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    setenv("OPAL_HOME",p.c_str(),1);
    return p;
}

template<class Fn>
static int run_with(Fn fn,const std::string&input,std::string*out=nullptr,std::string*err=nullptr){
    std::istringstream in(input);std::ostringstream capture,errors;
    auto*oldin=std::cin.rdbuf(in.rdbuf());auto*oldout=std::cout.rdbuf(capture.rdbuf());auto*olderr=std::cerr.rdbuf(errors.rdbuf());
    int rc=fn();
    std::cin.rdbuf(oldin);std::cout.rdbuf(oldout);std::cerr.rdbuf(olderr);
    if(out)*out=capture.str();if(err)*err=errors.str();return rc;
}

static int run_default(const std::string&input,std::string*out=nullptr,std::string*err=nullptr){return run_with([]{return opal::interactive_run();},input,out,err);}
static int run_default_stream(const opal::StreamOptions&stream,const std::string&input){return run_with([&]{return opal::interactive_run(stream);},input);}

int main(){
    {
        auto p=fresh("opal-setup-host-test");reset_calls();std::string out;
        assert(run_default("1\ny\n",&out)==0);
        opal::Ini c;assert(c.load(p/"config.ini"));assert(c.get("opal","role")=="host");
        assert(opal::host_setup_calls==1);assert(opal::tunnel_setup_calls==1);
        assert(opal::host_service_calls==1);assert(opal::tunnel_start_calls==0);assert(opal::host_run_calls==0);
        assert(out.find("opal:control-token")!=std::string::npos);
        assert(out.find("direct encrypted UDP")!=std::string::npos);
    }
    {
        auto p=fresh("opal-setup-client-test");reset_calls();
        assert(run_default("2\nopal:control-token\ndesktop\n")==0);
        opal::Ini c;assert(c.load(p/"config.ini"));assert(c.get("opal","role")=="client");assert(c.get("opal","default_host")=="desktop");
        assert(opal::add_calls==1);assert(opal::added_address=="opal:control-token");assert(opal::client_calls==1);assert(opal::client_target=="desktop");
        assert(opal::client_stream.max_width==1920&&opal::client_stream.max_height==1080&&opal::client_stream.fps==60);
    }
    {
        fresh("opal-setup-legacy-code-test");reset_calls();
        assert(run_default("2\nopal:control-token,legacy-video-token\ndesktop\n")==0);
        assert(opal::added_address=="opal:control-token,legacy-video-token");
        assert(opal::client_calls==1);
    }
    {
        fresh("opal-setup-invalid-code-test");reset_calls();std::string err;
        assert(run_default("2\nopal,opal-ctl-bad,opal-vid-bad\n",nullptr,&err)==2);
        assert(err.find("Invalid OPAL connection code. Expected: opal:CONTROL")!=std::string::npos);
        assert(opal::add_calls==0);assert(opal::client_calls==0);
    }
    {
        auto p=fresh("opal-auto-host-test");reset_calls();opal::Ini c;c.set("opal","role","host");c.save(p/"config.ini");
        assert(run_default("")==0);assert(opal::host_service_calls==1);assert(opal::tunnel_start_calls==0);assert(opal::host_run_calls==0);
    }
    {
        auto p=fresh("opal-auto-client-test");reset_calls();opal::Ini c;c.set("opal","role","client");c.set("opal","default_host","desktop");c.save(p/"config.ini");opal::Ini h;h.set("desktop","address","opal:control-token");h.set("desktop","mac","00:11:22:33:44:55");h.save(p/"hosts.ini");
        assert(run_default("")==0);assert(opal::wake_calls==1);assert(opal::wake_target=="desktop");assert(opal::client_calls==1);assert(opal::client_target=="desktop");
        assert(opal::client_stream.max_width==1920&&opal::client_stream.max_height==1080&&opal::client_stream.fps==60);
    }
    {
        auto p=fresh("opal-stream-override-test");reset_calls();opal::Ini c;c.set("opal","role","client");c.set("opal","default_host","desktop");c.save(p/"config.ini");opal::Ini h;h.set("desktop","address","opal:control-token");h.save(p/"hosts.ini");
        opal::StreamOptions stream{2560,1440,120};
        assert(run_default_stream(stream,"")==0);assert(opal::client_calls==1);assert(opal::client_stream.max_width==2560);assert(opal::client_stream.max_height==1440);assert(opal::client_stream.fps==120);
        opal::Ini saved;assert(saved.load(p/"config.ini"));assert(saved.get("opal","stream_width").empty());assert(saved.get("opal","stream_height").empty());assert(saved.get("opal","stream_fps").empty());
    }
    {
        auto p=fresh("opal-select-test");reset_calls();opal::Ini c;c.set("opal","role","client");c.set("opal","default_host","alpha");c.save(p/"config.ini");opal::Ini h;h.set("alpha","address","opal:control-a");h.set("beta","address","opal:control-b");h.save(p/"hosts.ini");
        assert(run_with([]{return opal::interactive_select();},"2\n")==0);opal::Ini selected;assert(selected.load(p/"config.ini"));assert(selected.get("opal","default_host")=="beta");assert(opal::client_calls==0);
    }
    {
        auto p=fresh("opal-remove-test");reset_calls();opal::Ini c;c.set("opal","role","client");c.set("opal","default_host","alpha");c.save(p/"config.ini");opal::Ini h;h.set("alpha","address","opal:control-a");h.set("beta","address","opal:control-b");h.save(p/"hosts.ini");
        assert(run_with([]{return opal::interactive_remove();},"1\n")==0);opal::Ini left;assert(left.load(p/"hosts.ini"));assert(left.sections().count("alpha")==0);assert(left.sections().count("beta")==1);opal::Ini selected;assert(selected.load(p/"config.ini"));assert(selected.get("opal","default_host")=="beta");
    }
    std::cout<<"setup tests passed\n";
}
