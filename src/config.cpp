#include <opal/config.hpp>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace opal {
std::string trim(std::string s) {
    auto first=s.find_first_not_of(" \t\r\n"); if(first==std::string::npos) return {};
    auto last=s.find_last_not_of(" \t\r\n"); return s.substr(first,last-first+1);
}
Paths Paths::load() {
    const char *override_home=std::getenv("OPAL_HOME");
    std::filesystem::path root;
    if(override_home && *override_home) root=override_home;
    else { const char *h=std::getenv("HOME"); root=(h&&*h)?std::filesystem::path(h)/".opal":std::filesystem::path(".opal"); }
    return {root,root/"config.ini",root/"hosts.ini",root/"host.ini",root/"identity.key",root/"identity.pub",root/"authorized_clients",root/"tls.crt",root/"tls.key",root/"logs"};
}
bool ensure_layout(const Paths &p) {
    std::error_code ec; std::filesystem::create_directories(p.root,ec); if(ec) return false;
    std::filesystem::create_directories(p.logs,ec); if(ec) return false;
    chmod(p.root.c_str(),0700); return true;
}
bool Ini::load(const std::filesystem::path &path) {
    data_.clear(); std::ifstream f(path); if(!f) return false; std::string section,line;
    while(std::getline(f,line)) { line=trim(line); if(line.empty()||line[0]=='#'||line[0]==';') continue;
        if(line.front()=='['&&line.back()==']'){section=trim(line.substr(1,line.size()-2));continue;}
        auto eq=line.find('='); if(eq==std::string::npos) continue; data_[section][trim(line.substr(0,eq))]=trim(line.substr(eq+1)); }
    return true;
}
bool Ini::save(const std::filesystem::path &path) const {
    std::ofstream f(path,std::ios::trunc); if(!f) return false;
    for(const auto &[section,values]:data_) { if(!section.empty()) f<<'['<<section<<"]\n"; for(const auto &[k,v]:values) f<<k<<'='<<v<<"\n"; f<<"\n"; }
    f.close(); chmod(path.c_str(),0600); return static_cast<bool>(f);
}
std::string Ini::get(const std::string&s,const std::string&k,const std::string&fb) const { auto si=data_.find(s); if(si==data_.end()) return fb; auto ki=si->second.find(k); return ki==si->second.end()?fb:ki->second; }
int Ini::get_int(const std::string&s,const std::string&k,int fb) const { try{return std::stoi(get(s,k,std::to_string(fb)));}catch(...){return fb;} }
bool Ini::get_bool(const std::string&s,const std::string&k,bool fb) const { auto v=get(s,k,fb?"true":"false"); return v=="1"||v=="true"||v=="yes"||v=="on"; }
void Ini::set(const std::string&s,const std::string&k,const std::string&v){data_[s][k]=v;}
std::string shell_quote(const std::string&s){std::string out="'";for(char c:s){if(c=='\'') out+="'\\''";else out+=c;}return out+="'";}
bool command_exists(const std::string &name){ auto cmd="command -v "+shell_quote(name)+" >/dev/null 2>&1"; return std::system(cmd.c_str())==0; }
}
