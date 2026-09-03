#include <opal/udp_transport.hpp>
#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <poll.h>
#include <random>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

namespace opal {
namespace {
constexpr std::uint32_t kStunMagic=0x2112A442u;

std::uint16_t read16(const std::uint8_t *p){
    std::uint16_t v=0;std::memcpy(&v,p,sizeof(v));return ntohs(v);
}
std::uint32_t read32(const std::uint8_t *p){
    std::uint32_t v=0;std::memcpy(&v,p,sizeof(v));return ntohl(v);
}
void put16(std::uint8_t *p,std::uint16_t v){
    v=htons(v);std::memcpy(p,&v,sizeof(v));
}
void put32(std::uint8_t *p,std::uint32_t v){
    v=htonl(v);std::memcpy(p,&v,sizeof(v));
}
void random_bytes(std::uint8_t *p,std::size_t size){
    std::random_device rd;
    for(std::size_t i=0;i<size;++i)p[i]=static_cast<std::uint8_t>(rd());
}
bool env_enabled(const char *name){
    const char *value=std::getenv(name);if(!value||!*value)return false;
    const std::string text=value;return text!="0"&&text!="false"&&text!="FALSE"&&text!="off"&&text!="OFF";
}

std::optional<UdpCandidate> parse_stun_response(
    const std::uint8_t *data,std::size_t size,const std::uint8_t tx[12]){
    if(size<20||size>1024)return std::nullopt;
    if(read16(data)!=0x0101||read32(data+4)!=kStunMagic)return std::nullopt;
    const std::size_t body_size=read16(data+2);
    if(body_size+20>size||std::memcmp(data+8,tx,12)!=0)return std::nullopt;
    const std::size_t end=20+body_size;

    for(std::size_t pos=20;pos<end;){
        if(pos+4>end)return std::nullopt;
        const std::uint16_t type=read16(data+pos);
        const std::uint16_t length=read16(data+pos+2);
        const std::size_t value=pos+4;
        if(value+length>end)return std::nullopt;

        if(type==0x0020&&length>=8){
            const std::uint8_t family=data[value+1];
            const std::uint16_t port=read16(data+value+2)^static_cast<std::uint16_t>(kStunMagic>>16);
            char text[INET6_ADDRSTRLEN]{};
            if(family==0x01&&length==8){
                const std::uint32_t host=read32(data+value+4)^kStunMagic;
                const std::uint32_t network=htonl(host);
                if(!inet_ntop(AF_INET,&network,text,sizeof(text)))return std::nullopt;
                return UdpCandidate{text,port,CandidateType::ServerReflexive};
            }
            if(family==0x02&&length==20){
                std::uint8_t address[16]{};
                constexpr std::uint8_t magic[4]={0x21,0x12,0xa4,0x42};
                for(int i=0;i<4;++i)address[i]=data[value+4+i]^magic[i];
                for(int i=4;i<16;++i)address[i]=data[value+4+i]^tx[i-4];
                if(!inet_ntop(AF_INET6,address,text,sizeof(text)))return std::nullopt;
                return UdpCandidate{text,port,CandidateType::ServerReflexive};
            }
        }

        pos=value+((static_cast<std::size_t>(length)+3u)&~3u);
    }
    return std::nullopt;
}
}

UdpSocket open_udp_socket(){
    const int fd=socket(AF_INET6,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if(fd<0)return {};
    int off=0;
    if(setsockopt(fd,IPPROTO_IPV6,IPV6_V6ONLY,&off,sizeof(off))!=0){close(fd);return {};}
    int queue_bytes=kUdpQueueBufferBytes;
    if(setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&queue_bytes,sizeof(queue_bytes))!=0||
       setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&queue_bytes,sizeof(queue_bytes))!=0){close(fd);return {};}
    int traffic_class=kUdpInteractiveTrafficClass;
    if(setsockopt(fd,IPPROTO_IPV6,IPV6_TCLASS,&traffic_class,sizeof(traffic_class))!=0){close(fd);return {};}
    // Best effort for IPv4-mapped destinations on the dual-stack socket.
    setsockopt(fd,IPPROTO_IP,IP_TOS,&traffic_class,sizeof(traffic_class));
    sockaddr_in6 address{};
    address.sin6_family=AF_INET6;
    address.sin6_addr=in6addr_any;
    if(bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0){
        close(fd);return {};
    }
    socklen_t length=sizeof(address);
    if(getsockname(fd,reinterpret_cast<sockaddr*>(&address),&length)!=0){
        close(fd);return {};
    }
    return {fd,ntohs(address.sin6_port)};
}

void close_udp_socket(UdpSocket &socket){
    if(socket.fd>=0)close(socket.fd);
    socket.fd=-1;
    socket.local_port=0;
}

std::vector<UdpCandidate> local_udp_candidates(const UdpSocket &socket){
    std::vector<UdpCandidate> candidates;
    if(socket.fd<0||socket.local_port==0)return candidates;
    ifaddrs *addresses=nullptr;
    if(getifaddrs(&addresses)!=0)return candidates;
    std::set<std::string> seen;

    for(auto *it=addresses;it;it=it->ifa_next){
        if(!it->ifa_addr||!(it->ifa_flags&IFF_UP))continue;
        const int family=it->ifa_addr->sa_family;
        char text[INET6_ADDRSTRLEN]{};
        if(family==AF_INET){
            auto *address=reinterpret_cast<sockaddr_in*>(it->ifa_addr);
            if(!inet_ntop(AF_INET,&address->sin_addr,text,sizeof(text)))continue;
        }else if(family==AF_INET6){
            auto *address=reinterpret_cast<sockaddr_in6*>(it->ifa_addr);
            if(IN6_IS_ADDR_UNSPECIFIED(&address->sin6_addr)||IN6_IS_ADDR_LINKLOCAL(&address->sin6_addr))continue;
            if(!inet_ntop(AF_INET6,&address->sin6_addr,text,sizeof(text)))continue;
        }else{
            continue;
        }
        if(seen.insert(text).second)candidates.push_back({text,socket.local_port,CandidateType::Local});
    }
    freeifaddrs(addresses);
    return candidates;
}

std::vector<StunEndpoint> default_stun_endpoints(){
    if(env_enabled("OPAL_DISABLE_STUN"))return {};
    return {{"stun.cloudflare.com",3478},{"stunserver2025.stunprotocol.org",3478}};
}

bool resolve_udp_endpoint(const std::string &host,std::uint16_t port,
                          sockaddr_storage &output,socklen_t &output_length){
    addrinfo hints{};
    hints.ai_socktype=SOCK_DGRAM;
    hints.ai_family=AF_UNSPEC;
    addrinfo *result=nullptr;
    const std::string service=std::to_string(port);
    if(getaddrinfo(host.c_str(),service.c_str(),&hints,&result)!=0)return false;

    bool resolved=false;
    for(auto *it=result;it&&!resolved;it=it->ai_next){
        if(it->ai_family==AF_INET6&&it->ai_addrlen<=sizeof(output)){
            std::memcpy(&output,it->ai_addr,it->ai_addrlen);
            output_length=static_cast<socklen_t>(it->ai_addrlen);
            resolved=true;
        }else if(it->ai_family==AF_INET){
            const auto *v4=reinterpret_cast<const sockaddr_in*>(it->ai_addr);
            sockaddr_in6 mapped{};
            mapped.sin6_family=AF_INET6;
            mapped.sin6_port=v4->sin_port;
            mapped.sin6_addr.s6_addr[10]=0xff;
            mapped.sin6_addr.s6_addr[11]=0xff;
            std::memcpy(mapped.sin6_addr.s6_addr+12,&v4->sin_addr,4);
            std::memcpy(&output,&mapped,sizeof(mapped));
            output_length=sizeof(mapped);
            resolved=true;
        }
    }
    freeaddrinfo(result);
    return resolved;
}

bool send_datagram(int fd,const sockaddr_storage &address,socklen_t address_length,
                   std::span<const std::uint8_t> data){
    if(fd<0||data.size()>65507)return false;
    const ssize_t written=sendto(fd,data.data(),data.size(),0,
        reinterpret_cast<const sockaddr*>(&address),address_length);
    return written==static_cast<ssize_t>(data.size());
}

int recv_datagram(int fd,std::span<std::uint8_t> data,sockaddr_storage &source,
                  socklen_t &source_length,int timeout_ms){
    if(fd<0||data.empty())return -1;
    pollfd descriptor{fd,POLLIN,0};
    int rc=0;
    do{rc=poll(&descriptor,1,std::max(0,timeout_ms));}while(rc<0&&errno==EINTR);
    if(rc==0)return -2;
    if(rc<0)return -1;
    source_length=sizeof(source);
    ssize_t received=0;
    do{
        received=recvfrom(fd,data.data(),data.size(),0,
            reinterpret_cast<sockaddr*>(&source),&source_length);
    }while(received<0&&errno==EINTR);
    if(received<0&&(errno==EAGAIN||errno==EWOULDBLOCK))return -2;
    return received<0?-1:static_cast<int>(received);
}

std::optional<UdpCandidate> discover_server_reflexive_candidate(
    const UdpSocket &socket,const std::vector<StunEndpoint> &endpoints,int timeout_ms){
    if(socket.fd<0||timeout_ms<=0||endpoints.empty())return std::nullopt;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout_ms);
    struct Pending {std::array<std::uint8_t,12> tx{};};
    std::vector<Pending> pending;pending.reserve(endpoints.size());

    for(const auto &endpoint:endpoints){
        if(std::chrono::steady_clock::now()>=deadline)break;
        sockaddr_storage target{};socklen_t target_length=0;
        if(!resolve_udp_endpoint(endpoint.host,endpoint.port,target,target_length))continue;
        std::uint8_t request[20]{};
        put16(request,0x0001);put16(request+2,0);put32(request+4,kStunMagic);random_bytes(request+8,12);
        if(!send_datagram(socket.fd,target,target_length,std::span<const std::uint8_t>(request,sizeof(request))))continue;
        Pending entry;std::copy_n(request+8,12,entry.tx.begin());pending.push_back(entry);
    }
    if(pending.empty())return std::nullopt;

    while(std::chrono::steady_clock::now()<deadline){
        const auto now=std::chrono::steady_clock::now();
        const int remaining=std::max(1,static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count()));
        std::uint8_t response[2048]{};sockaddr_storage source{};socklen_t source_length=sizeof(source);
        const int received=recv_datagram(socket.fd,response,source,source_length,remaining);
        if(received==-2)return std::nullopt;
        if(received<0)continue;
        for(const auto &entry:pending)
            if(auto mapped=parse_stun_response(response,static_cast<std::size_t>(received),entry.tx.data()))return mapped;
    }
    return std::nullopt;
}
}
