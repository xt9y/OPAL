#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace opal {

constexpr std::size_t kRendezvousMaxMessageBytes=1024;
constexpr std::size_t kRendezvousIdChars=12;

enum class RendezvousType : std::uint8_t {
    LeaseHello,
    LeaseChallenge,
    LeaseProof,
    LeaseOk,
    Introduce,
    Offer,
    Accept,
    Ready,
    RelayRequest,
    RelayReady,
    Error
};

struct RendezvousMessage {
    RendezvousType type=RendezvousType::Error;
    std::string id;
    std::string public_key;
    std::string nonce;
    std::string signature;
    std::string session_id;
    std::string host;
    std::string allocation_id;
    std::string error_code;
    std::uint16_t port=0;
    std::uint32_t ttl_seconds=0;
};

std::string rendezvous_id_from_public_key(std::string_view public_key_hex);
std::string format_connection_code(std::string_view rendezvous_id);
bool parse_connection_code(std::string_view code,std::string &rendezvous_id);

std::string serialize_rendezvous_message(const RendezvousMessage&);
bool parse_rendezvous_message(std::string_view wire,RendezvousMessage&);

}
