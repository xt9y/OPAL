#include <opal/session_packet.hpp>

#include <arpa/inet.h>
#include <cstring>

namespace opal { namespace {
void put16(std::uint8_t*p,std::uint16_t value){value=htons(value);std::memcpy(p,&value,2);}void put32(std::uint8_t*p,std::uint32_t value){value=htonl(value);std::memcpy(p,&value,4);}void put64(std::uint8_t*p,std::uint64_t value){for(int i=7;i>=0;--i){p[i]=static_cast<std::uint8_t>(value&0xff);value>>=8;}}
std::uint16_t get16(const std::uint8_t*p){std::uint16_t value=0;std::memcpy(&value,p,2);return ntohs(value);}std::uint32_t get32(const std::uint8_t*p){std::uint32_t value=0;std::memcpy(&value,p,4);return ntohl(value);}std::uint64_t get64(const std::uint8_t*p){std::uint64_t value=0;for(int i=0;i<8;++i)value=(value<<8)|p[i];return value;}
bool valid_type(SessionPacketType type){const auto value=static_cast<std::uint8_t>(type);return value>=static_cast<std::uint8_t>(SessionPacketType::HandshakeClient)&&value<=static_cast<std::uint8_t>(SessionPacketType::PathProbe);}
}

std::vector<std::uint8_t> serialize_session_header(const SessionPacketHeader&h){if(!valid_type(h.type)||h.generation==0||h.session_id==0||h.packet_sequence==0||h.payload_length>kSessionPacketMaxPayload)return {};if(h.type==SessionPacketType::ReliableControl&&h.reliable_sequence==0)return {};std::vector<std::uint8_t>out(kSessionPacketHeaderBytes);put32(out.data(),kSessionPacketMagic);out[4]=1;out[5]=static_cast<std::uint8_t>(h.type);out[6]=h.flags;out[7]=0;put32(out.data()+8,h.generation);put16(out.data()+12,h.payload_length);put16(out.data()+14,0);put64(out.data()+16,h.session_id);put64(out.data()+24,h.packet_sequence);put64(out.data()+32,h.reliable_sequence);put64(out.data()+40,h.ack_sequence);put32(out.data()+48,h.ack_bits);return out;}

bool parse_session_header(std::span<const std::uint8_t>wire,SessionPacketHeader&h){h={};if(wire.size()<kSessionPacketHeaderBytes||get32(wire.data())!=kSessionPacketMagic||wire[4]!=1||wire[7]!=0||get16(wire.data()+14)!=0)return false;const auto type=static_cast<SessionPacketType>(wire[5]);if(!valid_type(type))return false;h.type=type;h.flags=wire[6];h.generation=get32(wire.data()+8);h.payload_length=get16(wire.data()+12);h.session_id=get64(wire.data()+16);h.packet_sequence=get64(wire.data()+24);h.reliable_sequence=get64(wire.data()+32);h.ack_sequence=get64(wire.data()+40);h.ack_bits=get32(wire.data()+48);if(h.generation==0||h.session_id==0||h.packet_sequence==0||h.payload_length>kSessionPacketMaxPayload)return false;if(h.type==SessionPacketType::ReliableControl&&h.reliable_sequence==0)return false;return true;}

}
