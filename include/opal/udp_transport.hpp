#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include <sys/socket.h>

namespace opal {
constexpr int kUdpQueueBufferBytes=64*1024;
constexpr int kUdpReceiveQueueBufferBytes=4*1024*1024;
constexpr int kUdpInteractiveTrafficClass=0xb8; // DSCP EF, ECN bits clear
constexpr std::size_t kUdpReceiveBatchMax=32;

enum class UdpSendResult : std::uint8_t {
    Sent=0,
    WouldBlock,
    Fatal
};

struct UdpCandidate {
    std::string host;
    std::uint16_t port=0;
};

struct UdpSocket {
    int fd=-1;
    std::uint16_t local_port=0;
};

struct UdpReceiveSlot {
    std::span<std::uint8_t> buffer{};
    sockaddr_storage source{};
    socklen_t source_length=0;
    std::size_t size=0;
    std::uint32_t kernel_drops=0;
};

UdpSocket open_udp_socket();
void close_udp_socket(UdpSocket&);
std::vector<UdpCandidate> local_udp_candidates(const UdpSocket&);
bool resolve_udp_endpoint(const std::string&,std::uint16_t,sockaddr_storage&,socklen_t&);
UdpSendResult classify_udp_send_result(std::ptrdiff_t written,std::size_t expected,int error_number);
UdpSendResult send_datagram_result(int,const sockaddr_storage&,socklen_t,std::span<const std::uint8_t>);
bool send_datagram(int,const sockaddr_storage&,socklen_t,std::span<const std::uint8_t>);
int recv_datagram(int,std::span<std::uint8_t>,sockaddr_storage&,socklen_t&,int timeout_ms);
int recv_datagrams_batch(int,std::span<UdpReceiveSlot>,int timeout_ms);
}
