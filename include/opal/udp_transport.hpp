#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <sys/socket.h>

namespace opal {
enum class CandidateType { Local, ServerReflexive };

struct StunEndpoint {
    std::string host;
    std::uint16_t port=0;
};

struct UdpCandidate {
    std::string host;
    std::uint16_t port=0;
    CandidateType type=CandidateType::Local;
};

struct UdpSocket {
    int fd=-1;
    std::uint16_t local_port=0;
};

UdpSocket open_udp_socket();
void close_udp_socket(UdpSocket&);
std::vector<UdpCandidate> local_udp_candidates(const UdpSocket&);
std::vector<StunEndpoint> default_stun_endpoints();
std::optional<UdpCandidate> discover_server_reflexive_candidate(
    const UdpSocket&,const std::vector<StunEndpoint>&,int timeout_ms);
bool resolve_udp_endpoint(const std::string&,std::uint16_t,sockaddr_storage&,socklen_t&);
bool send_datagram(int,const sockaddr_storage&,socklen_t,std::span<const std::uint8_t>);
int recv_datagram(int,std::span<std::uint8_t>,sockaddr_storage&,socklen_t&,int timeout_ms);
}
