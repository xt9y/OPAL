#pragma once

#include <opal/udp_transport.hpp>
#include <cstdint>
#include <filesystem>
#include <string>

namespace opal {

constexpr std::uint16_t kTailnetDiscoveryPort=47993;

struct TailnetClientResult {
    UdpSocket socket;
    UdpCandidate host;
    std::string connection_id;
    std::string session_id;
    std::string host_public_key;
    std::string client_nonce;
    std::string host_nonce;
};

struct TailnetHostResult {
    UdpSocket socket;
    UdpCandidate client;
    std::string connection_id;
    std::string session_id;
    std::string client_public_key;
    std::string client_nonce;
    std::string host_nonce;
};

UdpSocket open_tailnet_discovery_listener(std::uint16_t port,std::string bind_host,std::string &error);

bool wait_tailnet_client(UdpSocket &listener,const std::string &host_public_key,
                         const std::filesystem::path &host_private_key,
                         TailnetHostResult &result,int timeout_ms,std::string &error);

bool discover_tailnet_host(const std::string &connection_id,const std::string &client_public_key,
                           const std::string &destination_host,TailnetClientResult &result,
                           std::string &error,int timeout_ms=3000,
                           std::uint16_t destination_port=kTailnetDiscoveryPort);

}
