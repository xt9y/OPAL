#include <opal/session_packet.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>

int main(){
    opal::SessionPacketHeader h;h.type=opal::SessionPacketType::ReliableControl;h.flags=3;h.generation=9;h.session_id=0x1122334455667788ULL;h.packet_sequence=123;h.reliable_sequence=44;h.ack_sequence=40;h.ack_bits=0x15;h.payload_length=321;
    std::array<std::uint8_t,opal::kSessionPacketHeaderBytes>fixed{};assert(opal::serialize_session_header(h,fixed));
    auto wire=opal::serialize_session_header(h);assert(wire.size()==opal::kSessionPacketHeaderBytes);assert(std::equal(wire.begin(),wire.end(),fixed.begin()));
    opal::SessionPacketHeader p;assert(opal::parse_session_header(wire,p));assert(p.type==h.type&&p.flags==h.flags&&p.generation==9&&p.session_id==h.session_id&&p.packet_sequence==123&&p.reliable_sequence==44&&p.ack_sequence==40&&p.ack_bits==0x15&&p.payload_length==321);
    auto corrupt=wire;corrupt[0]^=0xff;assert(!opal::parse_session_header(corrupt,p));
    std::array<std::uint8_t,opal::kSessionPacketHeaderBytes-1>small{};assert(!opal::serialize_session_header(h,small));
    h.generation=0;assert(opal::serialize_session_header(h).empty());h.generation=9;
    h.session_id=0;assert(opal::serialize_session_header(h).empty());h.session_id=1;
    h.packet_sequence=0;assert(opal::serialize_session_header(h).empty());h.packet_sequence=1;
    h.payload_length=opal::kSessionPacketMaxPayload+1;assert(opal::serialize_session_header(h).empty());
    std::uint8_t tiny[4]{};assert(!opal::parse_session_header(tiny,p));
    return 0;
}
