#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace opal {

constexpr std::uint32_t kSessionPacketMagic=0x4f504c34u; // OPL4
constexpr std::size_t kSessionPacketHeaderBytes=52;
constexpr std::size_t kSessionPacketMaxPayload=1050;
constexpr std::size_t kSessionPacketMaxBytes=kSessionPacketHeaderBytes+kSessionPacketMaxPayload+16;

enum class SessionPacketType : std::uint8_t {
    HandshakeClient=1,
    HandshakeHost=2,
    HandshakeFinish=3,
    ReliableControl=4,
    ControlAck=5,
    Pointer=6,
    Keepalive=7,
    PathProbe=8
};

struct SessionPacketHeader {
    SessionPacketType type=SessionPacketType::Keepalive;
    std::uint8_t flags=0;
    std::uint32_t generation=0;
    std::uint64_t session_id=0;
    std::uint64_t packet_sequence=0;
    std::uint64_t reliable_sequence=0;
    std::uint64_t ack_sequence=0;
    std::uint32_t ack_bits=0;
    std::uint16_t payload_length=0;
};

bool serialize_session_header(const SessionPacketHeader&,std::span<std::uint8_t>);
std::vector<std::uint8_t> serialize_session_header(const SessionPacketHeader&);
bool parse_session_header(std::span<const std::uint8_t>,SessionPacketHeader&);

}
