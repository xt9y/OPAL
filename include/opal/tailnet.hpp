#pragma once
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace opal {
constexpr int kTailnetDiscoveryTimeoutMs=3000;
constexpr int kTailnetPeerHandshakeTimeoutMs=6000;

inline bool is_tailnet_ipv4(const std::string&address){
    in_addr addr{};
    if(inet_pton(AF_INET,address.c_str(),&addr)!=1)return false;
    const auto value=ntohl(addr.s_addr);
    return (value&0xffc00000u)==0x64400000u;
}

inline std::vector<std::string> parse_tailnet_status_ipv4s(const std::string&status){
    std::vector<std::string>out;std::istringstream lines(status);std::string line;
    while(std::getline(lines,line)){
        std::istringstream row(line);std::string ip;
        if(!(row>>ip)||!is_tailnet_ipv4(ip))continue;
        if(std::find(out.begin(),out.end(),ip)==out.end())out.push_back(ip);
    }
    return out;
}

inline std::string first_tailnet_ipv4(const std::string&text){
    std::istringstream values(text);std::string value;
    while(values>>value)if(is_tailnet_ipv4(value))return value;
    return{};
}

inline std::string read_command_text(const char*command){
    if(!command||!*command)return{};
    FILE*pipe=popen(command,"r");if(!pipe)return{};
    std::array<char,512>buffer{};std::string text;
    while(fgets(buffer.data(),static_cast<int>(buffer.size()),pipe))text+=buffer.data();
    (void)pclose(pipe);return text;
}

inline std::vector<std::string> tailnet_peer_ipv4s(){
    return parse_tailnet_status_ipv4s(read_command_text("tailscale status --peers=true --self=false 2>/dev/null"));
}

inline std::string local_tailnet_ipv4(){
    if(auto cli=first_tailnet_ipv4(read_command_text("tailscale ip -4 2>/dev/null"));!cli.empty())return cli;

    // Linux exposes tailscale0; macOS GUI/Network Extension installs commonly
    // surface the tunnel through a utun device. Keep this only as a fallback
    // when the cross-platform Tailscale CLI cannot provide the local address.
    ifaddrs*interfaces=nullptr;
    if(getifaddrs(&interfaces)!=0)return {};
    std::string result;
    for(auto*it=interfaces;it;it=it->ifa_next){
        if(!it->ifa_addr||!it->ifa_name||it->ifa_addr->sa_family!=AF_INET)continue;
        const bool linux_tailscale=std::strcmp(it->ifa_name,"tailscale0")==0;
        const bool mac_tunnel=std::strncmp(it->ifa_name,"utun",4)==0;
        if(!linux_tailscale&&!mac_tunnel)continue;
        char text[INET_ADDRSTRLEN]{};
        const auto*addr=reinterpret_cast<const sockaddr_in*>(it->ifa_addr);
        if(!inet_ntop(AF_INET,&addr->sin_addr,text,sizeof(text)))continue;
        if(is_tailnet_ipv4(text)){result=text;break;}
    }
    freeifaddrs(interfaces);
    return result;
}
}
