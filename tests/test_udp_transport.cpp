#include <opal/udp_transport.hpp>
#include <arpa/inet.h>
#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
void put16(std::uint8_t *p,std::uint16_t v){v=htons(v);std::memcpy(p,&v,2);}
void put32(std::uint8_t *p,std::uint32_t v){v=htonl(v);std::memcpy(p,&v,4);}

std::uint16_t fake_stun_once(int mode){
    int fd=socket(AF_INET6,SOCK_DGRAM|SOCK_CLOEXEC,0);assert(fd>=0);
    sockaddr_in6 bind_addr{};bind_addr.sin6_family=AF_INET6;bind_addr.sin6_addr=in6addr_loopback;bind_addr.sin6_port=0;
    assert(bind(fd,reinterpret_cast<sockaddr*>(&bind_addr),sizeof(bind_addr))==0);
    socklen_t len=sizeof(bind_addr);assert(getsockname(fd,reinterpret_cast<sockaddr*>(&bind_addr),&len)==0);
    std::uint16_t port=ntohs(bind_addr.sin6_port);
    std::thread([fd,mode]{
        std::uint8_t request[256]{};sockaddr_storage peer{};socklen_t peer_len=sizeof(peer);
        ssize_t n=recvfrom(fd,request,sizeof(request),0,reinterpret_cast<sockaddr*>(&peer),&peer_len);
        if(n>=20){
            std::vector<std::uint8_t> response(mode==3?1100:32);
            put16(response.data(),0x0101);
            put16(response.data()+2,static_cast<std::uint16_t>(response.size()-20));
            put32(response.data()+4,0x2112A442);
            std::memcpy(response.data()+8,request+8,12);
            if(mode==1)response[8]^=0x80;
            put16(response.data()+20,0x0020);put16(response.data()+22,mode==2?9:8);
            response[24]=0;response[25]=0x01;
            put16(response.data()+26,static_cast<std::uint16_t>(45678u^(0x2112A442u>>16)));
            std::uint32_t addr=0;inet_pton(AF_INET,"203.0.113.7",&addr);
            put32(response.data()+28,ntohl(addr)^0x2112A442u);
            sendto(fd,response.data(),response.size(),0,reinterpret_cast<sockaddr*>(&peer),peer_len);
        }
        close(fd);
    }).detach();
    return port;
}

std::uint16_t fake_stun_silent(){
    int fd=socket(AF_INET6,SOCK_DGRAM|SOCK_CLOEXEC,0);assert(fd>=0);
    sockaddr_in6 bind_addr{};bind_addr.sin6_family=AF_INET6;bind_addr.sin6_addr=in6addr_loopback;bind_addr.sin6_port=0;
    assert(bind(fd,reinterpret_cast<sockaddr*>(&bind_addr),sizeof(bind_addr))==0);
    socklen_t len=sizeof(bind_addr);assert(getsockname(fd,reinterpret_cast<sockaddr*>(&bind_addr),&len)==0);
    std::uint16_t port=ntohs(bind_addr.sin6_port);
    std::thread([fd]{
        std::uint8_t request[64]{};sockaddr_storage peer{};socklen_t peer_len=sizeof(peer);
        recvfrom(fd,request,sizeof(request),0,reinterpret_cast<sockaddr*>(&peer),&peer_len);
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        close(fd);
    }).detach();
    return port;
}
}

int main(){
    auto a=opal::open_udp_socket();auto b=opal::open_udp_socket();
    assert(a.fd>=0&&a.local_port>0&&b.fd>=0&&b.local_port>0);

    int send_buffer=0,receive_buffer=0,traffic_class=0;socklen_t option_len=sizeof(int);
    assert(getsockopt(a.fd,SOL_SOCKET,SO_SNDBUF,&send_buffer,&option_len)==0);
    option_len=sizeof(int);assert(getsockopt(a.fd,SOL_SOCKET,SO_RCVBUF,&receive_buffer,&option_len)==0);
    option_len=sizeof(int);assert(getsockopt(a.fd,IPPROTO_IPV6,IPV6_TCLASS,&traffic_class,&option_len)==0);
    assert(send_buffer>=opal::kUdpQueueBufferBytes&&send_buffer<=opal::kUdpQueueBufferBytes*4);
    assert(receive_buffer>=opal::kUdpQueueBufferBytes&&receive_buffer<=opal::kUdpQueueBufferBytes*4);
    assert((traffic_class&0xfc)==opal::kUdpInteractiveTrafficClass);

    sockaddr_in6 dst{};dst.sin6_family=AF_INET6;dst.sin6_addr=in6addr_loopback;dst.sin6_port=htons(b.local_port);
    sockaddr_storage dst_storage{};std::memcpy(&dst_storage,&dst,sizeof(dst));
    const std::vector<std::uint8_t> payload={'O','P','A','L'};
    assert(opal::send_datagram(a.fd,dst_storage,sizeof(dst),payload));
    std::uint8_t receive[32]{};sockaddr_storage source{};socklen_t source_len=sizeof(source);
    int n=opal::recv_datagram(b.fd,receive,source,source_len,500);
    assert(n==4&&std::memcmp(receive,payload.data(),4)==0);

    std::array<std::array<std::uint8_t,32>,16> batch_buffers{};
    std::array<opal::UdpReceiveSlot,16> slots{};
    for(std::size_t i=0;i<slots.size();++i)slots[i].buffer=batch_buffers[i];
    for(std::uint8_t i=0;i<12;++i){const std::array<std::uint8_t,3> p={'B','T',i};assert(opal::send_datagram(a.fd,dst_storage,sizeof(dst),p));}
    const int batched=opal::recv_datagrams_batch(b.fd,slots,500);
    assert(batched>=2&&batched<=12);
    for(int i=0;i<batched;++i){assert(slots[static_cast<std::size_t>(i)].size==3);assert(slots[static_cast<std::size_t>(i)].buffer[0]=='B');assert(slots[static_cast<std::size_t>(i)].buffer[1]=='T');assert(slots[static_cast<std::size_t>(i)].source_length>0);}

    auto locals=opal::local_udp_candidates(a);assert(!locals.empty());
    for(const auto &candidate:locals)assert(candidate.port==a.local_port&&candidate.type==opal::CandidateType::Local);
    auto defaults=opal::default_stun_endpoints();assert(defaults.size()>=2);

    {
        std::uint16_t port=fake_stun_once(0);
        auto mapped=opal::discover_server_reflexive_candidate(a,{{"::1",port}},1000);
        assert(mapped&&mapped->type==opal::CandidateType::ServerReflexive);
        assert(mapped->host=="203.0.113.7"&&mapped->port==45678);
    }
    {
        const auto silent=fake_stun_silent();
        const auto good=fake_stun_once(0);
        const auto start=std::chrono::steady_clock::now();
        auto mapped=opal::discover_server_reflexive_candidate(a,{{"::1",silent},{"::1",good}},500);
        const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
        assert(mapped&&mapped->host=="203.0.113.7"&&mapped->port==45678);
        assert(elapsed<250);
    }
    {
        std::uint16_t port=fake_stun_once(1);
        assert(!opal::discover_server_reflexive_candidate(a,{{"::1",port}},150));
    }
    {
        std::uint16_t port=fake_stun_once(2);
        assert(!opal::discover_server_reflexive_candidate(a,{{"::1",port}},150));
    }
    {
        std::uint16_t port=fake_stun_once(3);
        assert(!opal::discover_server_reflexive_candidate(a,{{"::1",port}},150));
    }
    {
        auto start=std::chrono::steady_clock::now();
        assert(!opal::discover_server_reflexive_candidate(a,{{"::1",9}},120));
        auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
        assert(elapsed<600);
    }

    opal::close_udp_socket(a);opal::close_udp_socket(b);
    assert(a.fd<0&&b.fd<0);
    return 0;
}
