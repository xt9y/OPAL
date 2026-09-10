#pragma once

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace opal {
constexpr int kTailnetDiscoveryTimeoutMs=3000;
constexpr int kTailnetPeerHandshakeTimeoutMs=6000;

inline bool is_tailnet_ipv4(const std::string&address){
    int octets[4]{};std::size_t pos=0;
    for(int index=0;index<4;++index){
        if(pos>=address.size())return false;
        int value=0,digits=0;
        while(pos<address.size()&&address[pos]>='0'&&address[pos]<='9'){
            value=value*10+(address[pos]-'0');++pos;++digits;if(value>255)return false;
        }
        if(digits==0)return false;octets[index]=value;
        if(index<3){if(pos>=address.size()||address[pos]!='.')return false;++pos;}
    }
    return pos==address.size()&&octets[0]==100&&octets[1]>=64&&octets[1]<=127;
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

bool tailscale_cli_available();
bool tailscale_connected();
int require_tailscale();
std::vector<std::string> tailnet_peer_ipv4s();
std::string local_tailnet_ipv4();
}
