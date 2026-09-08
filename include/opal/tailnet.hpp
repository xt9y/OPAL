#pragma once
#include <algorithm>
#include <array>
#include <cstdio>
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
    return first_tailnet_ipv4(read_command_text("tailscale ip -4 2>/dev/null"));
}
}
