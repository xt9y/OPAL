#include <opal/local_discovery.hpp>
#include <opal/crypto.hpp>
#include <opal/rendezvous_protocol.hpp>

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <sstream>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

namespace opal { namespace {
using Clock=std::chrono::steady_clock;
constexpr std::size_t kLocalDiscoveryMessageBytes=768;

bool valid_public_key(const std::string &value){return unhex(value).size()==32;}
bool valid_id(const std::string &id){std::string checked;return parse_connection_code(format_connection_code(id),checked)&&checked==id;}
std::string offer_transcript(const std::string&id,const std::string&session_id,
                             const std::string&client_public_key,const std::string&client_nonce,
                             const std::string&host_public_key,const std::string&host_nonce,
                             std::uint16_t peer_port){
    return "OPAL-LOCAL-OFFER-v1\n"+id+"\n"+session_id+"\n"+client_public_key+"\n"+
           client_nonce+"\n"+host_public_key+"\n"+host_nonce+"\n"+std::to_string(peer_port);
}

UdpSocket open_dual_stack_listener(std::uint16_t port,const std::string&bind_host,std::string&error){
    error.clear();const int fd=socket(AF_INET6,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(fd<0){error="local discovery socket failed";return {};}
    int off=0;if(setsockopt(fd,IPPROTO_IPV6,IPV6_V6ONLY,&off,sizeof(off))!=0){close(fd);error="local discovery dual-stack unavailable";return {};}
    int one=1;(void)setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    int queue_bytes=kUdpQueueBufferBytes;if(setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&queue_bytes,sizeof(queue_bytes))!=0||setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&queue_bytes,sizeof(queue_bytes))!=0){close(fd);error="local discovery socket buffers unavailable";return {};}
#ifdef SO_RXQ_OVFL
    int overflow_reporting=1;(void)setsockopt(fd,SOL_SOCKET,SO_RXQ_OVFL,&overflow_reporting,sizeof(overflow_reporting));
#endif
    int traffic_class=kUdpInteractiveTrafficClass;if(setsockopt(fd,IPPROTO_IPV6,IPV6_TCLASS,&traffic_class,sizeof(traffic_class))!=0){close(fd);error="local discovery traffic class unavailable";return {};}(void)setsockopt(fd,IPPROTO_IP,IP_TOS,&traffic_class,sizeof(traffic_class));
    sockaddr_in6 address{};address.sin6_family=AF_INET6;address.sin6_port=htons(port);
    if(bind_host.empty()||bind_host=="0.0.0.0")address.sin6_addr=in6addr_any;
    else{
        in_addr ipv4{};if(inet_pton(AF_INET,bind_host.c_str(),&ipv4)!=1){close(fd);error="invalid local discovery bind address";return {};}
        address.sin6_addr=in6addr_any;address.sin6_addr.s6_addr[10]=0xff;address.sin6_addr.s6_addr[11]=0xff;std::memcpy(address.sin6_addr.s6_addr+12,&ipv4,sizeof(ipv4));
    }
    if(bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0){close(fd);error="local discovery bind failed";return {};}
    socklen_t length=sizeof(address);if(getsockname(fd,reinterpret_cast<sockaddr*>(&address),&length)!=0){close(fd);error="local discovery socket query failed";return {};}
    return {fd,ntohs(address.sin6_port)};
}

bool endpoint_from_sockaddr(const sockaddr_storage&source,socklen_t length,RendezvousEndpoint&endpoint){
    char host[NI_MAXHOST]{},service[NI_MAXSERV]{};if(getnameinfo(reinterpret_cast<const sockaddr*>(&source),length,host,sizeof(host),service,sizeof(service),NI_NUMERICHOST|NI_NUMERICSERV)!=0)return false;try{const int port=std::stoi(service);if(port<1||port>65535)return false;endpoint={host,static_cast<std::uint16_t>(port)};return true;}catch(...){return false;}
}

bool send_text(int fd,const sockaddr*target,socklen_t target_length,const std::string&text){return fd>=0&&!text.empty()&&sendto(fd,text.data(),text.size(),0,target,target_length)==static_cast<ssize_t>(text.size());}

bool parse_discover(std::string_view wire,std::string&id,std::string&client_public_key,std::string&client_nonce,std::uint16_t&peer_port){
    std::istringstream in{std::string(wire)};std::string word,port_text,extra;if(!(in>>word>>id>>client_public_key>>client_nonce>>port_text)||in>>extra||word!="OPAL_LOCAL_DISCOVER_V1")return false;
    try{size_t used=0;const unsigned long port=std::stoul(port_text,&used);if(used!=port_text.size()||port<1||port>65535)return false;peer_port=static_cast<std::uint16_t>(port);}catch(...){return false;}
    return valid_id(id)&&valid_public_key(client_public_key)&&unhex(client_nonce).size()==16;
}

bool parse_offer(std::string_view wire,std::string&id,std::string&session_id,std::string&host_public_key,
                 std::string&host_nonce,std::uint16_t&peer_port,std::string&signature){
    std::istringstream in{std::string(wire)};std::string word,port_text,extra;if(!(in>>word>>id>>session_id>>host_public_key>>host_nonce>>port_text>>signature)||in>>extra||word!="OPAL_LOCAL_OFFER_V1")return false;
    try{size_t used=0;const unsigned long port=std::stoul(port_text,&used);if(used!=port_text.size()||port<1||port>65535)return false;peer_port=static_cast<std::uint16_t>(port);}catch(...){return false;}
    return valid_id(id)&&unhex(session_id).size()==16&&valid_public_key(host_public_key)&&unhex(host_nonce).size()==16&&unhex(signature).size()==64;
}

int remaining_ms(Clock::time_point deadline){const auto now=Clock::now();if(now>=deadline)return 0;return std::max(1,static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count()));}
}

UdpSocket open_local_discovery_listener(std::uint16_t port,std::string bind_host,std::string &error){
    return open_dual_stack_listener(port,bind_host,error);
}

bool wait_local_client(UdpSocket &listener,const std::string &host_public_key,
                       const std::filesystem::path &host_private_key,
                       LocalDiscoveryHostResult &result,int timeout_ms,std::string &error){
    result={};error.clear();const auto id=rendezvous_id_from_public_key(host_public_key);if(listener.fd<0||listener.local_port==0||id.empty()||!valid_public_key(host_public_key)){error="local discovery listener unavailable";return false;}
    const auto deadline=Clock::now()+std::chrono::milliseconds(std::max(1,timeout_ms));std::array<std::uint8_t,kLocalDiscoveryMessageBytes>buffer{};
    while(remaining_ms(deadline)>0){sockaddr_storage source{};socklen_t source_len=sizeof(source);const int n=recv_datagram(listener.fd,buffer,source,source_len,remaining_ms(deadline));if(n==-2)continue;if(n<0){error="local discovery receive failed";return false;}if(n<=0||n>static_cast<int>(buffer.size()))continue;
        std::string request_id,client_public_key,client_nonce;std::uint16_t client_peer_port=0;if(!parse_discover(std::string_view(reinterpret_cast<const char*>(buffer.data()),static_cast<std::size_t>(n)),request_id,client_public_key,client_nonce,client_peer_port)||request_id!=id)continue;
        RendezvousEndpoint client;if(!endpoint_from_sockaddr(source,source_len,client))continue;
        if(client.port!=client_peer_port)continue;
        const int peer_fd=fcntl(listener.fd,F_DUPFD_CLOEXEC,0);if(peer_fd<0){error="local peer socket duplication failed";return false;}UdpSocket peer_socket{peer_fd,listener.local_port};
        const auto session_id=random_hex(16),host_nonce=random_hex(16);const auto transcript=offer_transcript(id,session_id,client_public_key,client_nonce,host_public_key,host_nonce,listener.local_port);const auto signature=sign_hex(host_private_key,transcript);if(signature.empty()){close_udp_socket(peer_socket);error="local discovery identity signing failed";return false;}
        const std::string offer="OPAL_LOCAL_OFFER_V1 "+id+" "+session_id+" "+host_public_key+" "+host_nonce+" "+std::to_string(listener.local_port)+" "+signature;
        if(!send_text(listener.fd,reinterpret_cast<const sockaddr*>(&source),source_len,offer)){close_udp_socket(peer_socket);continue;}
        result.socket=peer_socket;result.client=client;result.rendezvous_id=id;result.session_id=session_id;result.client_public_key=client_public_key;result.client_nonce=client_nonce;result.host_nonce=host_nonce;return true;
    }
    error="local discovery timeout";return false;
}

bool discover_local_host(const std::string &rendezvous_id,const std::string &client_public_key,
                         LocalDiscoveryClientResult &result,std::string &error,int timeout_ms,
                         std::string destination_host,std::uint16_t destination_port){
    result={};error.clear();if(!valid_id(rendezvous_id)||!valid_public_key(client_public_key)){error="invalid local discovery identity";return false;}
    // Keep discovery and PeerSession on the same dual-stack UDP socket. Peer
    // endpoints are normalized to AF_INET6 (IPv4-mapped when necessary), so
    // handing an AF_INET socket into PeerSession makes its first send fail.
    auto peer_socket=open_udp_socket();if(peer_socket.fd<0){error="local discovery socket failed";return false;}
    int one=1;if(setsockopt(peer_socket.fd,SOL_SOCKET,SO_BROADCAST,&one,sizeof(one))!=0){close_udp_socket(peer_socket);error="local discovery broadcast unavailable";return false;}
    sockaddr_storage target{};socklen_t target_length=0;if(!resolve_udp_endpoint(destination_host,destination_port,target,target_length)){close_udp_socket(peer_socket);error="local discovery destination unavailable";return false;}
    const auto client_nonce=random_hex(16);const std::string request="OPAL_LOCAL_DISCOVER_V1 "+rendezvous_id+" "+client_public_key+" "+client_nonce+" "+std::to_string(peer_socket.local_port);const auto deadline=Clock::now()+std::chrono::milliseconds(std::max(1,timeout_ms));auto next_send=Clock::time_point{};std::array<std::uint8_t,kLocalDiscoveryMessageBytes>buffer{};std::string rejection_error;
    while(remaining_ms(deadline)>0){const auto now=Clock::now();if(next_send.time_since_epoch().count()==0||now>=next_send){(void)send_datagram(peer_socket.fd,target,target_length,std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(request.data()),request.size()));next_send=now+std::chrono::milliseconds(75);}const int wait_ms=std::min(75,remaining_ms(deadline));sockaddr_storage source{};socklen_t source_len=sizeof(source);const int n=recv_datagram(peer_socket.fd,buffer,source,source_len,wait_ms);if(n==-2)continue;if(n<0){close_udp_socket(peer_socket);error="local discovery receive failed";return false;}if(n<=0||n>static_cast<int>(buffer.size()))continue;
        std::string id,session_id,host_public_key,host_nonce,signature;std::uint16_t host_peer_port=0;
        if(!parse_offer(std::string_view(reinterpret_cast<const char*>(buffer.data()),static_cast<std::size_t>(n)),id,session_id,host_public_key,host_nonce,host_peer_port,signature))continue;
        if(id!=rendezvous_id)continue;
        if(rendezvous_id_from_public_key(host_public_key)!=rendezvous_id){rejection_error="local discovery host identity mismatch";continue;}
        const auto transcript=offer_transcript(id,session_id,client_public_key,client_nonce,host_public_key,host_nonce,host_peer_port);
        if(!verify_hex(host_public_key,transcript,signature)){rejection_error="local discovery offer signature invalid";continue;}
        RendezvousEndpoint host;if(!endpoint_from_sockaddr(source,source_len,host)){rejection_error="local discovery offer source invalid";continue;}host.port=host_peer_port;
        result.socket=peer_socket;peer_socket={};result.host=host;result.rendezvous_id=id;result.session_id=session_id;result.host_public_key=host_public_key;result.client_nonce=client_nonce;result.host_nonce=host_nonce;return true;
    }
    close_udp_socket(peer_socket);error=rejection_error.empty()?"local host not found":rejection_error;return false;
}

}
