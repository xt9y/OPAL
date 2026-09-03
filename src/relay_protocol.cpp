#include <opal/relay_protocol.hpp>
#include <opal/crypto.hpp>

#include <arpa/inet.h>
#include <cstring>

namespace opal { namespace {
bool valid_role(RelayRole role){return role==RelayRole::Client||role==RelayRole::Host;}
void put16(std::uint8_t*p,std::uint16_t value){value=htons(value);std::memcpy(p,&value,2);}void put32(std::uint8_t*p,std::uint32_t value){value=htonl(value);std::memcpy(p,&value,4);}std::uint16_t get16(const std::uint8_t*p){std::uint16_t value=0;std::memcpy(&value,p,2);return ntohs(value);}std::uint32_t get32(const std::uint8_t*p){std::uint32_t value=0;std::memcpy(&value,p,4);return ntohl(value);}
}

std::string relay_request_transcript(std::string_view session_id,std::string_view public_key,std::string_view nonce){if(session_id.size()!=32||public_key.size()!=64||nonce.size()!=32)return {};return "OPAL-RELAY-REQUEST-v1\n"+std::string(session_id)+"\n"+std::string(public_key)+"\n"+std::string(nonce);}

std::vector<std::uint8_t> wrap_relay_datagram(std::string_view allocation_id,RelayRole role,std::span<const std::uint8_t>inner){
    const auto allocation=unhex(std::string(allocation_id));if(allocation.size()!=16||!valid_role(role)||inner.empty()||inner.size()>kRelayMaxInnerBytes)return {};std::vector<std::uint8_t>wire(kRelayHeaderBytes+inner.size());put32(wire.data(),kRelayMagic);wire[4]=1;wire[5]=static_cast<std::uint8_t>(role);put16(wire.data()+6,static_cast<std::uint16_t>(inner.size()));std::memcpy(wire.data()+8,allocation.data(),allocation.size());std::memcpy(wire.data()+kRelayHeaderBytes,inner.data(),inner.size());return wire;
}

bool parse_relay_datagram(std::span<const std::uint8_t>wire,RelayEnvelope&out){
    out={};if(wire.size()<kRelayHeaderBytes||get32(wire.data())!=kRelayMagic||wire[4]!=1)return false;const auto role=static_cast<RelayRole>(wire[5]);if(!valid_role(role))return false;const std::size_t inner_size=get16(wire.data()+6);if(inner_size==0||inner_size>kRelayMaxInnerBytes||wire.size()!=kRelayHeaderBytes+inner_size)return false;out.allocation_id=hex(wire.data()+8,16);out.role=role;out.inner=wire.subspan(kRelayHeaderBytes);return true;
}

}
