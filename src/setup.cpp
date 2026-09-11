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

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <libproc.h>
#include <mach-o/dyld.h>
#endif

#include <chrono>
#include <cerrno>
#include <csignal>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace opal {
namespace {

struct ClientRuntimeRecord {
    unsigned long long pid=0;
    std::string executable;
};

std::filesystem::path client_pid_path(const std::filesystem::path&root){return root/"client.pid";}

#if defined(_WIN32)
std::string utf8_from_wide(const wchar_t*value,int length){if(!value||length<=0)return{};const int bytes=WideCharToMultiByte(CP_UTF8,0,value,length,nullptr,0,nullptr,nullptr);if(bytes<=0)return{};std::string out(static_cast<std::size_t>(bytes),'\0');if(WideCharToMultiByte(CP_UTF8,0,value,length,out.data(),bytes,nullptr,nullptr)!=bytes)return{};return out;}
std::wstring wide_from_utf8(const std::string&value){if(value.empty())return{};const int chars=MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0);if(chars<=0)return{};std::wstring out(static_cast<std::size_t>(chars),L'\0');if(MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),out.data(),chars)!=chars)return{};return out;}
std::string current_executable_identity(){std::vector<wchar_t>buffer(1024);for(;;){const DWORD length=GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));if(!length)return{};if(length<buffer.size()-1)return utf8_from_wide(buffer.data(),static_cast<int>(length));if(buffer.size()>=32768)return{};buffer.resize(buffer.size()*2);}}
std::string process_executable_identity(unsigned long long pid){if(!pid||pid>0xffffffffULL)return{};HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,static_cast<DWORD>(pid));if(!process)return{};std::vector<wchar_t>buffer(32768);DWORD length=static_cast<DWORD>(buffer.size());const bool ok=QueryFullProcessImageNameW(process,0,buffer.data(),&length)!=FALSE&&length>0;CloseHandle(process);return ok?utf8_from_wide(buffer.data(),static_cast<int>(length)):std::string{};}
bool same_executable(const std::string&actual,const std::string&expected){const auto a=wide_from_utf8(actual),e=wide_from_utf8(expected);return!a.empty()&&!e.empty()&&CompareStringOrdinal(a.c_str(),-1,e.c_str(),-1,TRUE)==CSTR_EQUAL;}
unsigned long long current_pid(){return static_cast<unsigned long long>(GetCurrentProcessId());}
#else
std::string process_executable_identity(unsigned long long pid){
#if defined(__linux__)
    std::string link="/proc/"+std::to_string(pid)+"/exe";std::string buffer(4096,'\0');const auto size=readlink(link.c_str(),buffer.data(),buffer.size()-1);if(size<=0)return{};buffer.resize(static_cast<std::size_t>(size));constexpr const char deleted[]=" (deleted)";if(buffer.size()>sizeof(deleted)-1&&buffer.ends_with(deleted))buffer.resize(buffer.size()-(sizeof(deleted)-1));return buffer;
#elif defined(__APPLE__)
    if(!pid||pid>static_cast<unsigned long long>(std::numeric_limits<int>::max()))return{};char buffer[PROC_PIDPATHINFO_MAXSIZE]{};if(proc_pidpath(static_cast<int>(pid),buffer,sizeof(buffer))<=0)return{};return buffer;
#else
    (void)pid;return{};
#endif
}
std::string current_executable_identity(){
#if defined(__APPLE__)
    std::uint32_t size=0;(void)_NSGetExecutablePath(nullptr,&size);if(!size)return{};std::string buffer(static_cast<std::size_t>(size),'\0');if(_NSGetExecutablePath(buffer.data(),&size)!=0)return{};buffer.resize(std::char_traits<char>::length(buffer.c_str()));std::error_code error;const auto canonical=std::filesystem::weakly_canonical(buffer,error);return error?buffer:canonical.string();
#else
    return process_executable_identity(static_cast<unsigned long long>(getpid()));
#endif
}
bool same_executable(const std::string&actual,const std::string&expected){if(actual.empty()||expected.empty())return false;std::error_code ae,ee;const auto a=std::filesystem::weakly_canonical(actual,ae),e=std::filesystem::weakly_canonical(expected,ee);return(ae?std::filesystem::path(actual).lexically_normal():a)==(ee?std::filesystem::path(expected).lexically_normal():e);}
unsigned long long current_pid(){return static_cast<unsigned long long>(getpid());}
#endif

bool read_client_record(const std::filesystem::path&root,ClientRuntimeRecord&record){std::ifstream in(client_pid_path(root));if(!(in>>record.pid)||record.pid==0)return false;in.ignore(std::numeric_limits<std::streamsize>::max(),'\n');return static_cast<bool>(std::getline(in,record.executable))&&!record.executable.empty();}
void clear_client_record(const std::filesystem::path&root){std::error_code error;std::filesystem::remove(client_pid_path(root),error);}
bool client_process_matches(const ClientRuntimeRecord&record){if(!record.pid||record.pid==current_pid())return false;return same_executable(process_executable_identity(record.pid),record.executable);}

#if !defined(_WIN32)
bool client_process_alive(unsigned long long pid){if(!pid||pid>static_cast<unsigned long long>(std::numeric_limits<pid_t>::max()))return false;
#if defined(__linux__)
    std::ifstream stat("/proc/"+std::to_string(pid)+"/stat");std::string line;if(std::getline(stat,line)){const auto close=line.rfind(')');if(close!=std::string::npos&&close+2<line.size()&&line[close+2]=='Z')return false;}
#endif
    if(kill(static_cast<pid_t>(pid),0)==0)return true;return errno==EPERM;}
bool wait_for_client_exit(unsigned long long pid,std::chrono::milliseconds timeout){const auto deadline=std::chrono::steady_clock::now()+timeout;while(std::chrono::steady_clock::now()<deadline){if(!client_process_alive(pid))return true;std::this_thread::sleep_for(std::chrono::milliseconds(10));}return!client_process_alive(pid);}
#endif

class ClientRuntimeLease {
public:
    explicit ClientRuntimeLease(std::filesystem::path root):root_(std::move(root)){}
    ~ClientRuntimeLease(){if(!active_)return;ClientRuntimeRecord record;if(read_client_record(root_,record)&&record.pid==pid_)clear_client_record(root_);}
    bool acquire(){if(active_)return true;std::error_code error;std::filesystem::create_directories(root_,error);if(error)return false;ClientRuntimeRecord existing;if(read_client_record(root_,existing)){if(client_process_matches(existing))return false;clear_client_record(root_);}const auto executable=current_executable_identity();if(executable.empty())return false;pid_=current_pid();std::ofstream out(client_pid_path(root_),std::ios::out|std::ios::trunc);if(!out)return false;out<<pid_<<'\n'<<executable<<'\n';if(!out.good()){out.close();clear_client_record(root_);return false;}active_=true;return true;}
private:
    std::filesystem::path root_;unsigned long long pid_=0;bool active_=false;
};

bool stop_client_runtime(const std::filesystem::path&root){ClientRuntimeRecord record;if(!read_client_record(root,record)){clear_client_record(root);return true;}if(!client_process_matches(record)){clear_client_record(root);return true;}
#if defined(_WIN32)
    if(record.pid>0xffffffffULL)return false;HANDLE process=OpenProcess(PROCESS_TERMINATE|SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,static_cast<DWORD>(record.pid));if(!process)return false;std::vector<wchar_t>buffer(32768);DWORD length=static_cast<DWORD>(buffer.size());const bool identity_ok=QueryFullProcessImageNameW(process,0,buffer.data(),&length)!=FALSE&&length>0&&same_executable(utf8_from_wide(buffer.data(),static_cast<int>(length)),record.executable);if(!identity_ok){CloseHandle(process);clear_client_record(root);return true;}const bool terminated=TerminateProcess(process,0)!=FALSE;const bool exited=terminated&&WaitForSingleObject(process,3000)==WAIT_OBJECT_0;CloseHandle(process);if(!exited)return false;
#else
    if(record.pid>static_cast<unsigned long long>(std::numeric_limits<pid_t>::max()))return false;const auto pid=static_cast<pid_t>(record.pid);if(kill(pid,SIGTERM)!=0&&errno!=ESRCH)return false;if(!wait_for_client_exit(record.pid,std::chrono::milliseconds(1000))){if(kill(pid,SIGKILL)!=0&&errno!=ESRCH)return false;if(!wait_for_client_exit(record.pid,std::chrono::milliseconds(1000)))return false;}
#endif
    clear_client_record(root);return true;}

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
int tracked_client_connect(const std::string&target,const std::string&password,const StreamOptions&stream){auto p=Paths::load();if(!ensure_layout(p))return 1;ClientRuntimeLease lease(p.root);if(!lease.acquire()){std::cerr<<"OPAL client is already running or its runtime state could not be acquired. Use 'opal restart'.\n";return 1;}return client_connect(target,password,stream);}
int connect_default(const Ini &cfg,const StreamOptions &stream){auto name=cfg.get("opal","default_host");if(name.empty())return-1;auto p=Paths::load();Ini hosts;hosts.load(p.hosts);if(!hosts.get(name,"mac").empty()){std::cout<<"Waking "<<name<<"...\n";(void)wake_named(name);}std::cout<<"Connecting to "<<name<<"...\n";return tracked_client_connect(name,"",stream);}
int ensure_host_service(){if(host_service(true)!=0){std::cerr<<"Could not start OPAL host service. Run 'opal doctor' for platform diagnostics.\n";return 1;}std::cout<<"OPAL host service running.\n";return 0;}
int restart_host_service(){if(require_tailscale()!=0)return 1;if(host_service(false)!=0){std::cerr<<"Could not stop OPAL host service.\n";return 1;}if(host_service(true)!=0){std::cerr<<"Could not start OPAL host service.\n";return 1;}std::cout<<"OPAL host service restarted.\n";return 0;}

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
    if(choice=="2"){if(init()!=0)return 1;auto code=read_line("OPAL connection code: ");std::string id;if(!parse_connection_code(code,id)){std::cerr<<"Invalid OPAL connection code. Expected: XXXX-XXXX-XXXX\n";return 2;}auto name=read_line("Save as [desktop]: ","desktop");if(hosts_add(name,code)!=0||!save_role("client",name))return 1;return tracked_client_connect(name,"",stream);}return 0;
}
}

int interactive_setup(){return first_setup();}
int interactive_select(){auto p=Paths::load();Ini hosts;hosts.load(p.hosts);auto names=host_names(hosts);auto name=choose_host(hosts,"SELECT HOST");if(name.empty()){if(names.empty())print_local_host_credentials(p);return names.empty()?0:2;}ensure_layout(p);Ini cfg;cfg.load(p.config);cfg.set("opal","role","client");cfg.set("opal","default_host",name);if(!cfg.save(p.config))return 1;std::cout<<"Selected "<<name<<".\n";auto id=saved_connection_id(hosts,name);auto code=id.empty()?hosts.get(name,"connection_code"):format_connection_code(id);if(!code.empty())std::cout<<"Connection code: "<<code<<"\n";print_local_host_credentials(p);return 0;}
int interactive_remove(){auto p=Paths::load();Ini hosts;hosts.load(p.hosts);auto selected=choose_host(hosts,"REMOVE HOST");auto names=host_names(hosts);if(selected.empty())return names.empty()?0:2;Ini remaining;for(const auto &[name,values]:hosts.sections()){if(name.empty()||name==selected)continue;for(const auto &[key,value]:values)remaining.set(name,key,value);}if(!remaining.save(p.hosts))return 1;Ini cfg;cfg.load(p.config);if(cfg.get("opal","default_host")==selected){auto left=host_names(remaining);cfg.set("opal","default_host",left.empty()?"":left.front());if(!cfg.save(p.config))return 1;}std::cout<<"Removed "<<selected<<".\n";return 0;}
int interactive_stop(){auto p=Paths::load();if(!stop_client_runtime(p.root)){std::cerr<<"Could not stop OPAL client runtime.\n";return 1;}Ini cfg;cfg.load(p.config);const auto role=cfg.get("opal","role");const bool local_host=role=="host"||std::filesystem::exists(p.host);if(local_host&&host_service(false)!=0){std::cerr<<"Could not stop OPAL host service.\n";return 1;}std::cout<<"OPAL stopped.\n";return 0;}
int interactive_restart(const StreamOptions&stream){auto p=Paths::load();if(!stop_client_runtime(p.root)){std::cerr<<"Could not stop OPAL client runtime.\n";return 1;}Ini cfg;cfg.load(p.config);auto role=cfg.get("opal","role");if(role.empty()){Ini hosts;hosts.load(p.hosts);auto names=host_names(hosts);const bool local_host=std::filesystem::exists(p.host);if(names.size()==1&&!local_host){if(!save_role("client",names.front()))return 1;cfg.load(p.config);return connect_default(cfg,stream);}if(names.empty()&&local_host){if(!save_role("host"))return 1;return restart_host_service();}return first_setup(stream);}if(role=="client")return interactive_run(stream);if(role=="host")return restart_host_service();return first_setup(stream);}
int interactive_run(const StreamOptions &stream){auto p=Paths::load();Ini cfg;if(!cfg.load(p.config)||cfg.get("opal","role").empty())return first_setup(stream);auto role=cfg.get("opal","role");if(role=="host"){if(require_tailscale()!=0)return 1;return ensure_host_service();}if(role=="client"){int rc=connect_default(cfg,stream);if(rc==-1)return first_setup(stream);return rc;}return first_setup(stream);}
}
