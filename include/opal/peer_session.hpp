#pragma once

#include <opal/peer_handshake.hpp>
#include <opal/relay_protocol.hpp>
#include <opal/rendezvous_server.hpp>
#include <opal/udp_transport.hpp>
#include <opal/video_crypto.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace opal {

struct PeerRelayFallback {
    RendezvousEndpoint endpoint;
    std::string allocation_id;
    RelayRole role=RelayRole::Client;
};

struct PeerSessionOptions {
    bool client_side=false;
    UdpSocket socket;
    RendezvousEndpoint peer;
    std::optional<RendezvousEndpoint> lan_peer;
    std::optional<PeerRelayFallback> relay;
    PeerHandshakeContext handshake;
    std::filesystem::path identity_private_key;
    std::string pairing_password;
    int lan_handshake_timeout_ms=350;
    int direct_handshake_timeout_ms=2500;
    int relay_handshake_timeout_ms=5000;
    std::function<void(const std::string&)> reliable_input;
    std::function<void(const std::string&)> pointer_input;
    std::function<void(std::span<const std::uint8_t>)> media_datagram;
    // Only enable when media_datagram is strictly nonblocking and does not retain the span.
    // This removes the intermediate PeerSession media queue/copy/thread from the hot path.
    bool media_inline=false;
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
    UdpSendBatchResult send_media_datagrams(std::span<const std::span<const std::uint8_t>> wires);
    bool established() const;
    bool running() const;
    std::uint32_t generation() const;
    std::uint16_t local_port() const;
    std::uint64_t session_id() const;
    std::uint64_t reliable_pending() const;
    std::uint64_t pointer_sequence() const;
    std::uint64_t media_ingress_drops() const;
    std::uint64_t media_send_backpressure_drops() const;
    std::uint64_t control_dispatch_drops() const;
    std::uint64_t pointer_dispatch_overwrites() const;
    VideoKeys media_keys() const;
    std::string path_name() const;
    std::string last_error() const;
    void stop();
    ~PeerSession();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}