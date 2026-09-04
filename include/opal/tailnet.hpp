#pragma once
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <cstring>
#include <string>

namespace opal {
inline bool is_tailnet_ipv4(const std::string&address){
    in_addr addr{};
    if(inet_pton(AF_INET,address.c_str(),&addr)!=1)return false;
    const auto value=ntohl(addr.s_addr);
    return (value&0xffc00000u)==0x64400000u;
}

inline std::string local_tailnet_ipv4(){
    ifaddrs*interfaces=nullptr;
    if(getifaddrs(&interfaces)!=0)return {};
    std::string result;
    for(auto*it=interfaces;it;it=it->ifa_next){
        if(!it->ifa_addr||!it->ifa_name||it->ifa_addr->sa_family!=AF_INET)continue;
        if(std::strcmp(it->ifa_name,"tailscale0")!=0)continue;
        char text[INET_ADDRSTRLEN]{};
        const auto*addr=reinterpret_cast<const sockaddr_in*>(it->ifa_addr);
        if(!inet_ntop(AF_INET,&addr->sin_addr,text,sizeof(text)))continue;
        if(is_tailnet_ipv4(text)){result=text;break;}
    }
    freeifaddrs(interfaces);
    return result;
}
}
