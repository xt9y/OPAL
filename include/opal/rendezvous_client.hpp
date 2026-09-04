#pragma once

#include <opal/rendezvous_protocol.hpp>
#include <opal/rendezvous_server.hpp>
#include <opal/udp_transport.hpp>
#include <filesystem>
#include <memory>
#include <string>

namespace opal {

struct RendezvousConfig {
    std::string host="rendezvous.opal.xt9y.de";
    std::uint16_t port=47992;
    int timeout_ms=3000;
};

struct RendezvousIntroduction {
    std::string rendezvous_id;
    std::string session_id;
    std::string peer_public_key;
    std::string local_nonce;
    std::string peer_nonce;
    RendezvousEndpoint peer_observed;
    RendezvousEndpoint peer_local;
};

struct RelayAllocation {
    RendezvousEndpoint endpoint;
    std::string allocation_id;
    std::uint32_t ttl_seconds=0;
};

RendezvousConfig default_rendezvous_config();

class RendezvousClient {
public:
    RendezvousClient();
    RendezvousClient(const RendezvousClient&)=delete;
    RendezvousClient& operator=(const RendezvousClient&)=delete;
    bool open(const RendezvousConfig& config=default_rendezvous_config());
    bool register_host(const std::string &public_key,const std::filesystem::path &private_key,
                       std::string &rendezvous_id,std::uint32_t &lease_seconds,std::string &error);
    bool wait_offer(RendezvousMessage &offer,int timeout_ms,std::string &error);
    bool accept_offer(const RendezvousMessage &offer,const std::string &host_public_key,
                      const std::filesystem::path &host_private_key,RendezvousIntroduction &intro,
                      std::string &error);
    bool introduce(const std::string &rendezvous_id,const std::string &client_public_key,
                   RendezvousIntroduction &intro,std::string &error);
    bool request_relay(const std::string &session_id,const std::string &public_key,
                       const std::filesystem::path &private_key,RelayAllocation &allocation,
                       std::string &error);
    UdpSocket take_socket();
    std::uint16_t local_port() const;
    bool valid() const;
    void close();
    ~RendezvousClient();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
