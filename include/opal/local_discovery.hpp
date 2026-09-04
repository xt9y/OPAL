#pragma once

#include <opal/rendezvous_server.hpp>
#include <opal/udp_transport.hpp>
#include <cstdint>
#include <filesystem>
#include <string>

namespace opal {

constexpr std::uint16_t kLocalDiscoveryPort=47993;

struct LocalDiscoveryClientResult {
    UdpSocket socket;
    RendezvousEndpoint host;
    std::string rendezvous_id;
    std::string session_id;
    std::string host_public_key;
    std::string client_nonce;
    std::string host_nonce;
};

struct LocalDiscoveryHostResult {
    UdpSocket socket;
    RendezvousEndpoint client;
    std::string rendezvous_id;
    std::string session_id;
    std::string client_public_key;
    std::string client_nonce;
    std::string host_nonce;
};

UdpSocket open_local_discovery_listener(std::uint16_t port,std::string bind_host,std::string &error);

bool wait_local_client(UdpSocket &listener,const std::string &host_public_key,
                       const std::filesystem::path &host_private_key,
                       LocalDiscoveryHostResult &result,int timeout_ms,std::string &error);

bool discover_local_host(const std::string &rendezvous_id,const std::string &client_public_key,
                         LocalDiscoveryClientResult &result,std::string &error,int timeout_ms=300,
                         std::string destination_host="255.255.255.255",
                         std::uint16_t destination_port=kLocalDiscoveryPort);

}
