#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <opal/udp_transport.hpp>

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <poll.h>
#include <set>
#include <string>
#include <string_view>
#include <sys/uio.h>
#include <unistd.h>

namespace opal {
namespace {
bool unsuitable_lan_interface(const char*name,unsigned flags){
    if(!name||!*name||(flags&IFF_LOOPBACK)||(flags&IFF_POINTOPOINT))return true;
    const std::string_view n{name};
    constexpr std::string_view virtual_prefixes[]={"docker","veth","virbr","br-","podman","cni","flannel","tailscale","wg","tun","tap","zt"};
    for(const auto prefix:virtual_prefixes)if(n.starts_with(prefix))return true;
    return false;
}
}

UdpSocket open_udp_socket(){
    const int fd=socket(AF_INET6,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if(fd<0)return {};
    int off=0;
    if(setsockopt(fd,IPPROTO_IPV6,IPV6_V6ONLY,&off,sizeof(off))!=0){close(fd);return {};}
    int send_queue_bytes=kUdpQueueBufferBytes;
    int receive_queue_bytes=kUdpReceiveQueueBufferBytes;
    if(setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&send_queue_bytes,sizeof(send_queue_bytes))!=0||
       setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&receive_queue_bytes,sizeof(receive_queue_bytes))!=0){close(fd);return {};}
#ifdef SO_RXQ_OVFL
    int overflow_reporting=1;
    (void)setsockopt(fd,SOL_SOCKET,SO_RXQ_OVFL,&overflow_reporting,sizeof(overflow_reporting));
#endif
    int traffic_class=kUdpInteractiveTrafficClass;
    if(setsockopt(fd,IPPROTO_IPV6,IPV6_TCLASS,&traffic_class,sizeof(traffic_class))!=0){close(fd);return {};}
    (void)setsockopt(fd,IPPROTO_IP,IP_TOS,&traffic_class,sizeof(traffic_class));
    sockaddr_in6 address{};address.sin6_family=AF_INET6;address.sin6_addr=in6addr_any;
    if(bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0){close(fd);return {};}
    socklen_t length=sizeof(address);
    if(getsockname(fd,reinterpret_cast<sockaddr*>(&address),&length)!=0){close(fd);return {};}
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
        if(!it->ifa_addr||!(it->ifa_flags&IFF_UP)||unsuitable_lan_interface(it->ifa_name,it->ifa_flags))continue;
        const int family=it->ifa_addr->sa_family;
        char text[INET6_ADDRSTRLEN]{};
        if(family==AF_INET){
            const auto *address=reinterpret_cast<const sockaddr_in*>(it->ifa_addr);
            if(!inet_ntop(AF_INET,&address->sin_addr,text,sizeof(text)))continue;
        }else if(family==AF_INET6){
            const auto *address=reinterpret_cast<const sockaddr_in6*>(it->ifa_addr);
            if(IN6_IS_ADDR_UNSPECIFIED(&address->sin6_addr)||IN6_IS_ADDR_LOOPBACK(&address->sin6_addr)||IN6_IS_ADDR_LINKLOCAL(&address->sin6_addr))continue;
            if(!inet_ntop(AF_INET6,&address->sin6_addr,text,sizeof(text)))continue;
        }else continue;
        if(seen.insert(text).second)candidates.push_back({text,socket.local_port});
    }
    freeifaddrs(addresses);
    return candidates;
}

bool resolve_udp_endpoint(const std::string &host,std::uint16_t port,sockaddr_storage &output,socklen_t &output_length){
    if(host.empty()||port==0)return false;
    addrinfo hints{};hints.ai_socktype=SOCK_DGRAM;hints.ai_family=AF_UNSPEC;
    addrinfo *result=nullptr;const std::string service=std::to_string(port);
    if(getaddrinfo(host.c_str(),service.c_str(),&hints,&result)!=0)return false;
    bool resolved=false;
    for(auto *it=result;it&&!resolved;it=it->ai_next){
        if(it->ai_family==AF_INET6&&it->ai_addrlen<=sizeof(output)){std::memcpy(&output,it->ai_addr,it->ai_addrlen);output_length=static_cast<socklen_t>(it->ai_addrlen);resolved=true;}
        else if(it->ai_family==AF_INET){const auto *v4=reinterpret_cast<const sockaddr_in*>(it->ai_addr);sockaddr_in6 mapped{};mapped.sin6_family=AF_INET6;mapped.sin6_port=v4->sin_port;mapped.sin6_addr.s6_addr[10]=0xff;mapped.sin6_addr.s6_addr[11]=0xff;std::memcpy(mapped.sin6_addr.s6_addr+12,&v4->sin_addr,4);std::memcpy(&output,&mapped,sizeof(mapped));output_length=sizeof(mapped);resolved=true;}
    }
    freeaddrinfo(result);return resolved;
}

UdpSendResult classify_udp_send_result(std::ptrdiff_t written,std::size_t expected,int error_number){
    if(written==static_cast<std::ptrdiff_t>(expected))return UdpSendResult::Sent;
    if(written<0&&(error_number==EAGAIN||error_number==EWOULDBLOCK||error_number==ENOBUFS))return UdpSendResult::WouldBlock;
    return UdpSendResult::Fatal;
}

UdpSendResult send_datagram_result(int fd,const sockaddr_storage &address,socklen_t address_length,std::span<const std::uint8_t> data){
    if(fd<0||data.empty()||data.size()>65507)return UdpSendResult::Fatal;
    ssize_t written=0;
    do{written=sendto(fd,data.data(),data.size(),MSG_DONTWAIT,reinterpret_cast<const sockaddr*>(&address),address_length);}while(written<0&&errno==EINTR);
    const int send_errno=written<0?errno:0;
    return classify_udp_send_result(static_cast<std::ptrdiff_t>(written),data.size(),send_errno);
}

bool send_datagram(int fd,const sockaddr_storage &address,socklen_t address_length,std::span<const std::uint8_t> data){return send_datagram_result(fd,address,address_length,data)==UdpSendResult::Sent;}

int recv_datagram(int fd,std::span<std::uint8_t> data,sockaddr_storage &source,socklen_t &source_length,int timeout_ms){
    if(fd<0||data.empty())return -1;
    pollfd descriptor{fd,POLLIN,0};int rc=0;do{rc=poll(&descriptor,1,std::max(0,timeout_ms));}while(rc<0&&errno==EINTR);
    if(rc==0)return -2;if(rc<0||!(descriptor.revents&POLLIN))return -1;
    source_length=sizeof(source);ssize_t received=0;do{received=recvfrom(fd,data.data(),data.size(),0,reinterpret_cast<sockaddr*>(&source),&source_length);}while(received<0&&errno==EINTR);
    if(received<0&&(errno==EAGAIN||errno==EWOULDBLOCK))return -2;
    return received<0?-1:static_cast<int>(received);
}

int recv_datagrams_batch(int fd,std::span<UdpReceiveSlot> slots,int timeout_ms){
    if(fd<0||slots.empty())return -1;
    const std::size_t count=std::min(slots.size(),kUdpReceiveBatchMax);
    for(std::size_t i=0;i<count;++i){if(slots[i].buffer.empty())return -1;slots[i].size=0;slots[i].source_length=0;slots[i].kernel_drops=0;}
    pollfd descriptor{fd,POLLIN,0};int rc=0;do{rc=poll(&descriptor,1,std::max(0,timeout_ms));}while(rc<0&&errno==EINTR);
    if(rc==0)return 0;if(rc<0||!(descriptor.revents&POLLIN))return -1;
#if defined(__linux__)
    std::array<mmsghdr,kUdpReceiveBatchMax> messages{};std::array<iovec,kUdpReceiveBatchMax> vectors{};
#ifdef SO_RXQ_OVFL
    constexpr std::size_t control_bytes=CMSG_SPACE(sizeof(std::uint32_t));std::array<std::array<unsigned char,control_bytes>,kUdpReceiveBatchMax> controls{};
#endif
    for(std::size_t i=0;i<count;++i){vectors[i].iov_base=slots[i].buffer.data();vectors[i].iov_len=slots[i].buffer.size();messages[i].msg_hdr.msg_name=&slots[i].source;messages[i].msg_hdr.msg_namelen=sizeof(slots[i].source);messages[i].msg_hdr.msg_iov=&vectors[i];messages[i].msg_hdr.msg_iovlen=1;
#ifdef SO_RXQ_OVFL
        messages[i].msg_hdr.msg_control=controls[i].data();messages[i].msg_hdr.msg_controllen=controls[i].size();
#endif
    }
    int received=0;do{received=recvmmsg(fd,messages.data(),static_cast<unsigned int>(count),MSG_DONTWAIT,nullptr);}while(received<0&&errno==EINTR);
    if(received<0&&(errno==EAGAIN||errno==EWOULDBLOCK))return 0;if(received<0)return -1;
    for(int i=0;i<received;++i){auto &slot=slots[static_cast<std::size_t>(i)];slot.size=messages[static_cast<std::size_t>(i)].msg_len;slot.source_length=messages[static_cast<std::size_t>(i)].msg_hdr.msg_namelen;
#ifdef SO_RXQ_OVFL
        for(cmsghdr *cmsg=CMSG_FIRSTHDR(&messages[static_cast<std::size_t>(i)].msg_hdr);cmsg;cmsg=CMSG_NXTHDR(&messages[static_cast<std::size_t>(i)].msg_hdr,cmsg)){if(cmsg->cmsg_level==SOL_SOCKET&&cmsg->cmsg_type==SO_RXQ_OVFL&&cmsg->cmsg_len>=CMSG_LEN(sizeof(std::uint32_t))){std::uint32_t drops=0;std::memcpy(&drops,CMSG_DATA(cmsg),sizeof(drops));slot.kernel_drops=drops;break;}}
#endif
    }
    return received;
#else
    int received=0;for(std::size_t i=0;i<count;++i){auto &slot=slots[i];socklen_t length=sizeof(slot.source);const int n=recv_datagram(fd,slot.buffer,slot.source,length,i==0?timeout_ms:0);if(n==-2)break;if(n<0)return received?received:-1;slot.size=static_cast<std::size_t>(n);slot.source_length=length;++received;}return received;
#endif
}

}
