#include <opal/setup.hpp>
#include <opal/config.hpp>
#include <opal/media_profile.hpp>
#include <opal/rendezvous_protocol.hpp>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

namespace opal {
int interactive_select();int interactive_remove();
int init_calls=0,host_setup_calls=0,host_run_calls=0,host_service_calls=0,client_calls=0,wake_calls=0,add_calls=0;std::string client_target,added_address,wake_target;StreamOptions client_stream;
int init(){++init_calls;ensure_layout(Paths::load());return 0;}int doctor(){return 0;}int host_setup(){++host_setup_calls;ensure_layout(Paths::load());Ini h;h.set("host","password","TEST");h.save(Paths::load().host);std::cout<<"OPAL host ready.\n";return 0;}int host_run(){++host_run_calls;return 0;}int host_service(bool){++host_service_calls;return 0;}int bridge_setup(const char*){return 0;}int hosts_list(){return 0;}
int hosts_add(const std::string&name,const std::string&address,const std::string&mac){++add_calls;added_address=address;std::string id;assert(parse_connection_code(address,id));Ini h;h.load(Paths::load().hosts);h.set(name,"rendezvous_id",id);if(!mac.empty())h.set(name,"mac",mac);h.save(Paths::load().hosts);return 0;}
int client_connect(const std::string&target,const std::string&password,const StreamOptions&stream){(void)password;++client_calls;client_target=target;client_stream=stream;return 0;}int client_connect(const std::string&target,const std::string&password){return client_connect(target,password,{});}int wake_named(const std::string&name){++wake_calls;wake_target=name;return 0;}
}

static std::string code(){const std::string pub="00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";return opal::format_connection_code(opal::rendezvous_id_from_public_key(pub));}
static void reset_calls(){opal::init_calls=opal::host_setup_calls=opal::host_run_calls=opal::host_service_calls=opal::client_calls=opal::wake_calls=opal::add_calls=0;opal::client_target.clear();opal::added_address.clear();opal::wake_target.clear();opal::client_stream={};}
static std::filesystem::path fresh(const char*name){auto p=std::filesystem::temp_directory_path()/name;std::filesystem::remove_all(p);std::filesystem::create_directories(p);setenv("OPAL_HOME",p.c_str(),1);return p;}
template<class Fn>static int run_with(Fn fn,const std::string&input,std::string*out=nullptr,std::string*err=nullptr){std::istringstream in(input);std::ostringstream capture,errors;auto*oldin=std::cin.rdbuf(in.rdbuf());auto*oldout=std::cout.rdbuf(capture.rdbuf());auto*olderr=std::cerr.rdbuf(errors.rdbuf());int rc=fn();std::cin.rdbuf(oldin);std::cout.rdbuf(oldout);std::cerr.rdbuf(olderr);if(out)*out=capture.str();if(err)*err=errors.str();return rc;}
static int run_default(const std::string&input,std::string*out=nullptr,std::string*err=nullptr){return run_with([]{return opal::interactive_run();},input,out,err);}static int run_default_stream(const opal::StreamOptions&stream,const std::string&input){return run_with([&]{return opal::interactive_run(stream);},input);}

int main(){const auto native_code=code();assert(native_code.rfind("opal:",0)==0);
    {auto p=fresh("opal-setup-host-test");reset_calls();assert(run_default("1\ny\n")==0);opal::Ini c;assert(c.load(p/"config.ini"));assert(c.get("opal","role")=="host");assert(opal::host_setup_calls==1&&opal::host_service_calls==1&&opal::host_run_calls==0);}
    {auto p=fresh("opal-setup-client-test");reset_calls();assert(run_default("2\n"+native_code+"\ndesktop\n")==0);opal::Ini c;assert(c.load(p/"config.ini"));assert(c.get("opal","role")=="client"&&c.get("opal","default_host")=="desktop");assert(opal::add_calls==1&&opal::added_address==native_code&&opal::client_calls==1&&opal::client_target=="desktop");assert(opal::client_stream.max_width==1920&&opal::client_stream.max_height==1080&&opal::client_stream.fps==60);}
    {fresh("opal-setup-legacy-code-test");reset_calls();std::string err;assert(run_default("2\nopal:control-token,legacy-video-token\n",nullptr,&err)==2);assert(err.find("opal:XXXX-XXXX-XXXX")!=std::string::npos);assert(opal::add_calls==0&&opal::client_calls==0);}
    {fresh("opal-setup-invalid-code-test");reset_calls();std::string err;assert(run_default("2\nopal,opal-ctl-bad,opal-vid-bad\n",nullptr,&err)==2);assert(err.find("opal:XXXX-XXXX-XXXX")!=std::string::npos);assert(opal::add_calls==0&&opal::client_calls==0);}
    {auto p=fresh("opal-auto-host-test");reset_calls();opal::Ini c;c.set("opal","role","host");c.save(p/"config.ini");assert(run_default("")==0);assert(opal::host_service_calls==1&&opal::host_run_calls==0);}
    {auto p=fresh("opal-auto-client-test");reset_calls();opal::Ini c;c.set("opal","role","client");c.set("opal","default_host","desktop");c.save(p/"config.ini");opal::Ini h;std::string id;assert(opal::parse_connection_code(native_code,id));h.set("desktop","rendezvous_id",id);h.set("desktop","mac","00:11:22:33:44:55");h.save(p/"hosts.ini");assert(run_default("")==0);assert(opal::wake_calls==1&&opal::wake_target=="desktop"&&opal::client_calls==1&&opal::client_target=="desktop");}
    {auto p=fresh("opal-stream-override-test");reset_calls();opal::Ini c;c.set("opal","role","client");c.set("opal","default_host","desktop");c.save(p/"config.ini");opal::Ini h;std::string id;opal::parse_connection_code(native_code,id);h.set("desktop","rendezvous_id",id);h.save(p/"hosts.ini");opal::StreamOptions stream{2560,1440,120};assert(run_default_stream(stream,"")==0);assert(opal::client_calls==1&&opal::client_stream.max_width==2560&&opal::client_stream.max_height==1440&&opal::client_stream.fps==120);}
    {auto p=fresh("opal-select-test");reset_calls();opal::Ini c;c.set("opal","role","client");c.set("opal","default_host","alpha");c.save(p/"config.ini");opal::Ini h;std::string id;opal::parse_connection_code(native_code,id);h.set("alpha","rendezvous_id",id);h.set("beta","rendezvous_id",id);h.save(p/"hosts.ini");assert(run_with([]{return opal::interactive_select();},"2\n")==0);opal::Ini selected;assert(selected.load(p/"config.ini")&&selected.get("opal","default_host")=="beta");}
    {auto p=fresh("opal-remove-test");reset_calls();opal::Ini c;c.set("opal","role","client");c.set("opal","default_host","alpha");c.save(p/"config.ini");opal::Ini h;std::string id;opal::parse_connection_code(native_code,id);h.set("alpha","rendezvous_id",id);h.set("beta","rendezvous_id",id);h.save(p/"hosts.ini");assert(run_with([]{return opal::interactive_remove();},"1\n")==0);opal::Ini left;assert(left.load(p/"hosts.ini")&&left.sections().count("alpha")==0&&left.sections().count("beta")==1);}
    std::cout<<"setup tests passed\n";
}
