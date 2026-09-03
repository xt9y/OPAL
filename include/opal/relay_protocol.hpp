#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace opal {

constexpr std::uint32_t kRelayMagic=0x4f50524cu; // OPRL
constexpr std::size_t kRelayHeaderBytes=24;
constexpr std::size_t kRelayMaxInnerBytes=1200;

enum class RelayRole : std::uint8_t { Client=1, Host=2 };

struct RelayEnvelope {
    std::string allocation_id;
    RelayRole role=RelayRole::Client;
    std::span<const std::uint8_t> inner;
};

std::string relay_request_transcript(std::string_view session_id,std::string_view public_key,
                                     std::string_view nonce);
std::vector<std::uint8_t> wrap_relay_datagram(std::string_view allocation_id,RelayRole,
                                              std::span<const std::uint8_t> inner);
bool parse_relay_datagram(std::span<const std::uint8_t> wire,RelayEnvelope&);

}
