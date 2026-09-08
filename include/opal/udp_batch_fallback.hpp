#pragma once

#include <opal/udp_transport.hpp>

#include <cstddef>
#include <span>

namespace opal {

template <class SendOne>
UdpSendBatchResult portable_udp_batch_send(
    std::span<const std::span<const std::uint8_t>> datagrams,
    SendOne &&send_one)
{
    if (datagrams.empty() || datagrams.size() > kUdpSendBatchMax) return {};
    for (const auto datagram : datagrams) {
        if (datagram.empty() || datagram.size() > 65507) return {};
    }

    std::size_t sent = 0;
    for (const auto datagram : datagrams) {
        const auto result = send_one(datagram);
        if (result != UdpSendResult::Sent) return {sent, result};
        ++sent;
    }
    return {sent, UdpSendResult::Sent};
}

}
