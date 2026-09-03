#pragma once

#include <opal/relay_protocol.hpp>
#include <opal/rendezvous_protocol.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace opal {

struct RendezvousEndpoint {
    std::string host;
    std::uint16_t port=0;
    bool operator==(const RendezvousEndpoint&) const=default;
};

struct RendezvousOutbound {
    RendezvousEndpoint target;
    RendezvousMessage message;
};

class RendezvousServerState {
public:
    RendezvousServerState();
    explicit RendezvousServerState(RendezvousEndpoint relay_endpoint);
    RendezvousServerState(const RendezvousServerState&)=delete;
    RendezvousServerState& operator=(const RendezvousServerState&)=delete;
    std::vector<RendezvousOutbound> process(const RendezvousMessage&,const RendezvousEndpoint&,std::uint64_t now_ms);
    bool relay_target(std::string_view allocation_id,RelayRole,const RendezvousEndpoint&source,
                      std::uint64_t now_ms,RendezvousEndpoint&target);
    void cleanup(std::uint64_t now_ms);
    std::size_t active_leases(std::uint64_t now_ms);
    std::size_t pending_introductions(std::uint64_t now_ms);
    ~RendezvousServerState();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
