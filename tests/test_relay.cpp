#include <opal/relay_protocol.hpp>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

int main(){
    const std::string allocation="00112233445566778899aabbccddeeff";
    const std::vector<std::uint8_t> inner={0x4f,0x50,0x4c,0x34,0xde,0xad,0xbe,0xef};

    auto wire=opal::wrap_relay_datagram(allocation,opal::RelayRole::Client,inner);
    assert(wire.size()==opal::kRelayHeaderBytes+inner.size());
    opal::RelayEnvelope envelope;
    assert(opal::parse_relay_datagram(wire,envelope));
    assert(envelope.allocation_id==allocation);
    assert(envelope.role==opal::RelayRole::Client);
    assert(std::vector<std::uint8_t>(envelope.inner.begin(),envelope.inner.end())==inner);

    auto host_wire=opal::wrap_relay_datagram(allocation,opal::RelayRole::Host,inner);
    assert(opal::parse_relay_datagram(host_wire,envelope)&&envelope.role==opal::RelayRole::Host);

    assert(opal::wrap_relay_datagram("bad",opal::RelayRole::Client,inner).empty());
    assert(opal::wrap_relay_datagram(allocation,static_cast<opal::RelayRole>(3),inner).empty());
    assert(opal::wrap_relay_datagram(allocation,opal::RelayRole::Client,{}).empty());
    std::vector<std::uint8_t> oversized(opal::kRelayMaxInnerBytes+1,0x41);
    assert(opal::wrap_relay_datagram(allocation,opal::RelayRole::Client,oversized).empty());

    auto bad=wire;bad[0]^=0xff;assert(!opal::parse_relay_datagram(bad,envelope));
    bad=wire;bad[4]=2;assert(!opal::parse_relay_datagram(bad,envelope));
    bad=wire;bad[5]=3;assert(!opal::parse_relay_datagram(bad,envelope));
    bad=wire;bad.pop_back();assert(!opal::parse_relay_datagram(bad,envelope));

    const std::string session(32,'1'),key(64,'2'),nonce(32,'3');
    const auto transcript=opal::relay_request_transcript(session,key,nonce);
    assert(!transcript.empty());
    assert(transcript.find(session)!=std::string::npos&&transcript.find(key)!=std::string::npos&&transcript.find(nonce)!=std::string::npos);
    assert(opal::relay_request_transcript(session.substr(1),key,nonce).empty());
    assert(opal::relay_request_transcript(session,key.substr(1),nonce).empty());
    assert(opal::relay_request_transcript(session,key,nonce.substr(1)).empty());
    return 0;
}
