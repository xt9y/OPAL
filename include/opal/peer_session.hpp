#pragma once

#include <opal/peer_handshake.hpp>
#include <opal/rendezvous_server.hpp>
#include <opal/udp_transport.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace opal {

struct PeerSessionOptions {
    bool client_side=false;
    UdpSocket socket;
    RendezvousEndpoint peer;
    PeerHandshakeContext handshake;
    std::filesystem::path identity_private_key;
    std::string pairing_password;
    int handshake_timeout_ms=5000;
    std::function<void(const std::string&)> reliable_input;
    std::function<void(const std::string&)> pointer_input;
    std::function<void(std::span<const std::uint8_t>)> media_datagram;
};

class PeerSession {
public:
    PeerSession();
    PeerSession(const PeerSession&)=delete;
    PeerSession& operator=(const PeerSession&)=delete;
    bool start(PeerSessionOptions,std::string &error);
    bool send_input(std::string command);
    bool send_pointer(std::string command);
    bool send_media_datagram(std::span<const std::uint8_t> wire);
    bool established() const;
    bool running() const;
    std::uint32_t generation() const;
    std::uint16_t local_port() const;
    std::uint64_t reliable_pending() const;
    std::uint64_t pointer_sequence() const;
    std::string last_error() const;
    void stop();
    ~PeerSession();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
