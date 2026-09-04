#include <opal/relay_protocol.hpp>
#include <opal/rendezvous_protocol.hpp>
#include <opal/rendezvous_server.hpp>

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {
std::atomic<bool> running{true};
void stop_signal(int){running.store(false);}
std::uint64_t monotonic_ms(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
int bind_udp(const std::string&host,std::uint16_t port){addrinfo hints{};hints.ai_family=AF_UNSPEC;hints.ai_socktype=SOCK_DGRAM;hints.ai_flags=AI_PASSIVE;addrinfo*result=nullptr;const auto service=std::to_string(port);if(getaddrinfo(host.empty()?nullptr:host.c_str(),service.c_str(),&hints,&result)!=0)return -1;int fd=-1;for(auto*it=result;it&&fd<0;it=it->ai_next){int candidate=socket(it->ai_family,SOCK_DGRAM|SOCK_CLOEXEC,0);if(candidate<0)continue;int one=1;setsockopt(candidate,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));if(it->ai_family==AF_INET6){int off=0;setsockopt(candidate,IPPROTO_IPV6,IPV6_V6ONLY,&off,sizeof(off));}if(bind(candidate,it->ai_addr,it->ai_addrlen)==0)fd=candidate;else close(candidate);}freeaddrinfo(result);return fd;}
bool endpoint_from_sockaddr(const sockaddr_storage&source,socklen_t length,opal::RendezvousEndpoint&endpoint){char host[NI_MAXHOST]{},service[NI_MAXSERV]{};if(getnameinfo(reinterpret_cast<const sockaddr*>(&source),length,host,sizeof(host),service,sizeof(service),NI_NUMERICHOST|NI_NUMERICSERV)!=0)return false;try{const int port=std::stoi(service);if(port<1||port>65535)return false;endpoint={host,static_cast<std::uint16_t>(port)};return true;}catch(...){return false;}}
bool sockaddr_from_endpoint(const opal::RendezvousEndpoint&endpoint,sockaddr_storage&out,socklen_t&length){addrinfo hints{};hints.ai_family=AF_UNSPEC;hints.ai_socktype=SOCK_DGRAM;hints.ai_flags=AI_NUMERICHOST|AI_NUMERICSERV;addrinfo*result=nullptr;const auto service=std::to_string(endpoint.port);if(getaddrinfo(endpoint.host.c_str(),service.c_str(),&hints,&result)!=0)return false;bool ok=false;for(auto*it=result;it&&!ok;it=it->ai_next)if(it->ai_addrlen<=sizeof(out)){std::memcpy(&out,it->ai_addr,it->ai_addrlen);length=static_cast<socklen_t>(it->ai_addrlen);ok=true;}freeaddrinfo(result);return ok;}
bool send_endpoint(int fd,const opal::RendezvousEndpoint&endpoint,const void*data,std::size_t size){sockaddr_storage target{};socklen_t target_len=0;if(!sockaddr_from_endpoint(endpoint,target,target_len))return false;return sendto(fd,data,size,0,reinterpret_cast<const sockaddr*>(&target),target_len)==static_cast<ssize_t>(size);}
}

int main(int argc,char**argv){
    std::string bind_host="::";std::uint16_t port=47992;if(const char*v=std::getenv("OPAL_RENDEZVOUS_BIND");v&&*v)bind_host=v;if(const char*v=std::getenv("OPAL_RENDEZVOUS_PORT");v&&*v)try{const int p=std::stoi(v);if(p>0&&p<=65535)port=static_cast<std::uint16_t>(p);}catch(...){return 2;}if(argc>1)bind_host=argv[1];if(argc>2)try{const int p=std::stoi(argv[2]);if(p<1||p>65535)return 2;port=static_cast<std::uint16_t>(p);}catch(...){return 2;}
    std::string public_host;if(const char*v=std::getenv("OPAL_RENDEZVOUS_PUBLIC_HOST");v&&*v)public_host=v;else if(bind_host!="::"&&bind_host!="0.0.0.0")public_host=bind_host;else public_host="rendezvous.opal.xt9y.de";
    const int fd=bind_udp(bind_host,port);if(fd<0){std::cerr<<"cannot bind OPAL rendezvous server\n";return 1;}signal(SIGINT,stop_signal);signal(SIGTERM,stop_signal);opal::RendezvousServerState state({public_host,port});std::cout<<"OPAL rendezvous+relay listening on "<<bind_host<<":"<<port<<" public="<<public_host<<":"<<port<<"\n"<<std::flush;
    constexpr std::size_t kBufferBytes=opal::kRelayHeaderBytes+opal::kRelayMaxInnerBytes;
    while(running.load()){
        std::uint8_t buffer[kBufferBytes+1]{};sockaddr_storage source{};socklen_t source_len=sizeof(source);const ssize_t n=recvfrom(fd,buffer,sizeof(buffer),0,reinterpret_cast<sockaddr*>(&source),&source_len);if(n<0){if(errno==EINTR)continue;break;}if(n<=0||n>static_cast<ssize_t>(kBufferBytes))continue;opal::RendezvousEndpoint endpoint;if(!endpoint_from_sockaddr(source,source_len,endpoint))continue;const auto now=monotonic_ms();const std::span<const std::uint8_t>bytes(buffer,static_cast<std::size_t>(n));
        opal::RelayEnvelope relay;if(opal::parse_relay_datagram(bytes,relay)){opal::RendezvousEndpoint target;if(state.relay_target(relay.allocation_id,relay.role,endpoint,now,target))(void)send_endpoint(fd,target,relay.inner.data(),relay.inner.size());continue;}
        if(n>static_cast<ssize_t>(opal::kRendezvousMaxMessageBytes))continue;
        opal::RendezvousMessage message;
        if(!opal::parse_rendezvous_message(std::string_view(reinterpret_cast<const char*>(buffer),static_cast<std::size_t>(n)),message))continue;
        const auto outputs=state.process(message,endpoint,now);
        for(const auto&output:outputs){
            const auto wire=opal::serialize_rendezvous_message(output.message);
            if(!wire.empty())(void)send_endpoint(fd,output.target,wire.data(),wire.size());
        }
    }
    close(fd);return 0;
}
