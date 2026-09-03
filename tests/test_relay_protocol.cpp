#include <opal/relay_protocol.hpp>
#include <cassert>
#include <vector>

int main(){
    const std::string allocation="00112233445566778899aabbccddeeff";const std::vector<std::uint8_t>inner={1,2,3,4,5};
    auto wire=opal::wrap_relay_datagram(allocation,opal::RelayRole::Client,inner);assert(wire.size()==opal::kRelayHeaderBytes+inner.size());
    opal::RelayEnvelope parsed;assert(opal::parse_relay_datagram(wire,parsed));assert(parsed.allocation_id==allocation&&parsed.role==opal::RelayRole::Client&&std::vector<std::uint8_t>(parsed.inner.begin(),parsed.inner.end())==inner);
    auto host_wire=opal::wrap_relay_datagram(allocation,opal::RelayRole::Host,inner);assert(opal::parse_relay_datagram(host_wire,parsed)&&parsed.role==opal::RelayRole::Host);
    assert(opal::wrap_relay_datagram("bad",opal::RelayRole::Client,inner).empty());
    assert(opal::wrap_relay_datagram(allocation,static_cast<opal::RelayRole>(9),inner).empty());
    assert(opal::wrap_relay_datagram(allocation,opal::RelayRole::Client,std::vector<std::uint8_t>(opal::kRelayMaxInnerBytes+1)).empty());
    auto corrupt=wire;corrupt[0]^=0xff;assert(!opal::parse_relay_datagram(corrupt,parsed));
    assert(!opal::parse_relay_datagram(std::span<const std::uint8_t>(wire.data(),opal::kRelayHeaderBytes-1),parsed));
    assert(opal::relay_request_transcript(std::string(32,'1'),std::string(64,'2'),std::string(32,'3')).find("OPAL-RELAY-REQUEST-v1")!=std::string::npos);
    return 0;
}
