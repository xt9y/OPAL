#include <opal/udp_transport.hpp>
#include <arpa/inet.h>
#include <array>
#include <cassert>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

int main(){
    auto a=opal::open_udp_socket();auto b=opal::open_udp_socket();
    assert(a.fd>=0&&a.local_port>0&&b.fd>=0&&b.local_port>0);

    int send_buffer=0,receive_buffer=0,traffic_class=0;socklen_t option_len=sizeof(int);
    assert(getsockopt(a.fd,SOL_SOCKET,SO_SNDBUF,&send_buffer,&option_len)==0);
    option_len=sizeof(int);assert(getsockopt(a.fd,SOL_SOCKET,SO_RCVBUF,&receive_buffer,&option_len)==0);
    option_len=sizeof(int);assert(getsockopt(a.fd,IPPROTO_IPV6,IPV6_TCLASS,&traffic_class,&option_len)==0);
    assert(opal::kUdpReceiveQueueBufferBytes>=1024*1024);
    assert(opal::kUdpReceiveQueueBufferBytes>opal::kUdpQueueBufferBytes);
    assert(send_buffer>=opal::kUdpQueueBufferBytes&&send_buffer<=opal::kUdpQueueBufferBytes*4);
    assert(receive_buffer>=send_buffer);
    assert(receive_buffer>=256*1024);
    assert((traffic_class&0xfc)==opal::kUdpInteractiveTrafficClass);

    sockaddr_storage dst_storage{};socklen_t dst_len=0;
    assert(opal::resolve_udp_endpoint("::1",b.local_port,dst_storage,dst_len));
    const std::vector<std::uint8_t> payload={'O','P','A','L'};
    assert(opal::send_datagram(a.fd,dst_storage,dst_len,payload));
    std::uint8_t receive[32]{};sockaddr_storage source{};socklen_t source_len=sizeof(source);
    int n=opal::recv_datagram(b.fd,receive,source,source_len,500);
    assert(n==4&&std::memcmp(receive,payload.data(),4)==0);

    sockaddr_storage mapped_v4{};socklen_t mapped_v4_len=0;
    assert(opal::resolve_udp_endpoint("127.0.0.1",b.local_port,mapped_v4,mapped_v4_len));
    assert(mapped_v4.ss_family==AF_INET6);
    const auto*mapped=reinterpret_cast<const sockaddr_in6*>(&mapped_v4);
    assert(IN6_IS_ADDR_V4MAPPED(&mapped->sin6_addr));

    std::array<std::array<std::uint8_t,32>,16> batch_buffers{};
    std::array<opal::UdpReceiveSlot,16> slots{};
    for(std::size_t i=0;i<slots.size();++i)slots[i].buffer=batch_buffers[i];
    for(std::uint8_t i=0;i<12;++i){const std::array<std::uint8_t,3> p={'B','T',i};assert(opal::send_datagram(a.fd,dst_storage,dst_len,p));}
    const int batched=opal::recv_datagrams_batch(b.fd,slots,500);
    assert(batched>=1&&batched<=12);
    for(int i=0;i<batched;++i){assert(slots[static_cast<std::size_t>(i)].size==3);assert(slots[static_cast<std::size_t>(i)].buffer[0]=='B');assert(slots[static_cast<std::size_t>(i)].buffer[1]=='T');assert(slots[static_cast<std::size_t>(i)].source_length>0);}

    const auto locals=opal::local_udp_candidates(a);
    for(const auto &candidate:locals){
        assert(candidate.port==a.local_port);
        assert(candidate.host!="127.0.0.1"&&candidate.host!="::1");
        assert(!candidate.host.empty());
    }

    std::array<std::uint8_t,1> empty_buffer{};
    assert(!opal::send_datagram(a.fd,dst_storage,dst_len,std::span<const std::uint8_t>{}));
    sockaddr_storage none{};socklen_t none_len=0;assert(!opal::resolve_udp_endpoint("",1234,none,none_len));assert(!opal::resolve_udp_endpoint("::1",0,none,none_len));

    opal::close_udp_socket(a);opal::close_udp_socket(b);
    assert(a.fd<0&&b.fd<0);
    (void)empty_buffer;
    return 0;
}
