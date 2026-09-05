#include <opal/udp_transport.hpp>
#include <arpa/inet.h>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <span>
#include <string>
#include <unistd.h>
#include <vector>

int main(){
    assert(opal::classify_udp_send_result(4,4,0)==opal::UdpSendResult::Sent);
    assert(opal::classify_udp_send_result(-1,4,EAGAIN)==opal::UdpSendResult::WouldBlock);
    assert(opal::classify_udp_send_result(-1,4,EWOULDBLOCK)==opal::UdpSendResult::WouldBlock);
    assert(opal::classify_udp_send_result(-1,4,ENOBUFS)==opal::UdpSendResult::WouldBlock);
    assert(opal::classify_udp_send_result(-1,4,ECONNREFUSED)==opal::UdpSendResult::Fatal);
    assert(opal::classify_udp_send_result(3,4,0)==opal::UdpSendResult::Fatal);

    auto a=opal::open_udp_socket();auto b=opal::open_udp_socket();
    assert(a.fd>=0&&a.local_port>0&&b.fd>=0&&b.local_port>0);

    int send_buffer=0,receive_buffer=0,traffic_class=0;socklen_t option_len=sizeof(int);
    assert(getsockopt(a.fd,SOL_SOCKET,SO_SNDBUF,&send_buffer,&option_len)==0);
    option_len=sizeof(int);assert(getsockopt(a.fd,SOL_SOCKET,SO_RCVBUF,&receive_buffer,&option_len)==0);
    option_len=sizeof(int);assert(getsockopt(a.fd,IPPROTO_IPV6,IPV6_TCLASS,&traffic_class,&option_len)==0);
    assert(opal::kUdpReceiveQueueBufferBytes>=256*1024);
    assert(opal::kUdpReceiveQueueBufferBytes<=512*1024);
    assert(opal::kUdpReceiveQueueBufferBytes>opal::kUdpQueueBufferBytes);
    assert(send_buffer>=opal::kUdpQueueBufferBytes&&send_buffer<=opal::kUdpQueueBufferBytes*4);
    assert(receive_buffer>=send_buffer);
    assert(receive_buffer>=256*1024);
    assert(receive_buffer<=opal::kUdpReceiveQueueBufferBytes*4);
    assert((traffic_class&0xfc)==opal::kUdpInteractiveTrafficClass);

    sockaddr_storage dst_storage{};socklen_t dst_len=0;
    assert(opal::resolve_udp_endpoint("::1",b.local_port,dst_storage,dst_len));
    const std::vector<std::uint8_t> payload={'O','P','A','L'};
    assert(opal::send_datagram_result(a.fd,dst_storage,dst_len,payload)==opal::UdpSendResult::Sent);
    assert(opal::send_datagram(a.fd,dst_storage,dst_len,payload));
    std::uint8_t receive[32]{};sockaddr_storage source{};socklen_t source_len=sizeof(source);
    int n=opal::recv_datagram(b.fd,receive,source,source_len,500);
    assert(n==4&&std::memcmp(receive,payload.data(),4)==0);
    n=opal::recv_datagram(b.fd,receive,source,source_len,500);
    assert(n==4&&std::memcmp(receive,payload.data(),4)==0);

    std::array<std::array<std::uint8_t,4>,8> send_batch{};
    std::array<std::span<const std::uint8_t>,8> send_views{};
    for(std::size_t i=0;i<send_batch.size();++i){send_batch[i]={'S','M','M',static_cast<std::uint8_t>(i)};send_views[i]=send_batch[i];}
    const auto send_result=opal::send_datagrams_batch(a.fd,dst_storage,dst_len,send_views);
    assert(send_result.result==opal::UdpSendResult::Sent);
    assert(send_result.sent==send_views.size());

    std::array<std::array<std::uint8_t,32>,16> batch_buffers{};
    std::array<opal::UdpReceiveSlot,16> slots{};
    for(std::size_t i=0;i<slots.size();++i)slots[i].buffer=batch_buffers[i];
    std::size_t received_batch=0;
    while(received_batch<send_views.size()){
        const int got=opal::recv_datagrams_batch(b.fd,slots,500);
        assert(got>0);
        for(int i=0;i<got;++i){
            const auto index=received_batch+static_cast<std::size_t>(i);
            assert(index<send_views.size());
            const auto&slot=slots[static_cast<std::size_t>(i)];
            assert(slot.size==4&&slot.buffer[0]=='S'&&slot.buffer[1]=='M'&&slot.buffer[2]=='M');
            assert(slot.buffer[3]==static_cast<std::uint8_t>(index));
            assert(slot.source_length>0);
        }
        received_batch+=static_cast<std::size_t>(got);
    }

    sockaddr_storage mapped_v4{};socklen_t mapped_v4_len=0;
    assert(opal::resolve_udp_endpoint("127.0.0.1",b.local_port,mapped_v4,mapped_v4_len));
    assert(mapped_v4.ss_family==AF_INET6);
    const auto*mapped=reinterpret_cast<const sockaddr_in6*>(&mapped_v4);
    assert(IN6_IS_ADDR_V4MAPPED(&mapped->sin6_addr));

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

    assert(opal::send_datagram_result(a.fd,dst_storage,dst_len,std::span<const std::uint8_t>{})==opal::UdpSendResult::Fatal);
    assert(!opal::send_datagram(a.fd,dst_storage,dst_len,std::span<const std::uint8_t>{}));
    const std::array<std::span<const std::uint8_t>,1> empty_batch={std::span<const std::uint8_t>{}};
    assert(opal::send_datagrams_batch(a.fd,dst_storage,dst_len,empty_batch).result==opal::UdpSendResult::Fatal);
    assert(opal::send_datagrams_batch(-1,dst_storage,dst_len,send_views).result==opal::UdpSendResult::Fatal);
    sockaddr_storage none{};socklen_t none_len=0;assert(!opal::resolve_udp_endpoint("",1234,none,none_len));assert(!opal::resolve_udp_endpoint("::1",0,none,none_len));

    opal::close_udp_socket(a);opal::close_udp_socket(b);
    assert(a.fd<0&&b.fd<0);
    return 0;
}
