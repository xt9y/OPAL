#include <opal/rendezvous_protocol.hpp>
#include <cassert>
#include <string>

int main(){
    const std::string pub="00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    const auto id=opal::rendezvous_id_from_public_key(pub);
    assert(id.size()==opal::kRendezvousIdChars);
    const auto code=opal::format_connection_code(id);
    assert(code.rfind("opal:",0)==0);
    assert(code.size()==5+4+1+4+1+4);
    std::string parsed;
    assert(opal::parse_connection_code(code,parsed)&&parsed==id);
    auto damaged=code;damaged.back()=damaged.back()=='0'?'1':'0';
    assert(!opal::parse_connection_code(damaged,parsed));
    assert(!opal::parse_connection_code("opal:AAAA-BBBB",parsed));
    assert(opal::rendezvous_id_from_public_key("not-a-key").empty());

    auto roundtrip=[](opal::RendezvousMessage in){
        const auto wire=opal::serialize_rendezvous_message(in);assert(!wire.empty());assert(wire.size()<=opal::kRendezvousMaxMessageBytes);
        opal::RendezvousMessage out;assert(opal::parse_rendezvous_message(wire,out));return out;
    };

    opal::RendezvousMessage hello;hello.type=opal::RendezvousType::LeaseHello;hello.id=id;hello.public_key=pub;
    auto hello2=roundtrip(hello);assert(hello2.type==hello.type&&hello2.id==id&&hello2.public_key==pub);

    opal::RendezvousMessage challenge;challenge.type=opal::RendezvousType::LeaseChallenge;challenge.id=id;challenge.nonce=std::string(32,'a');
    assert(roundtrip(challenge).nonce==challenge.nonce);

    opal::RendezvousMessage proof;proof.type=opal::RendezvousType::LeaseProof;proof.id=id;proof.public_key=pub;proof.nonce=std::string(32,'b');proof.signature=std::string(128,'c');
    assert(roundtrip(proof).signature==proof.signature);

    opal::RendezvousMessage ok;ok.type=opal::RendezvousType::LeaseOk;ok.id=id;ok.ttl_seconds=45;
    assert(roundtrip(ok).ttl_seconds==45);

    opal::RendezvousMessage intro;intro.type=opal::RendezvousType::Introduce;intro.id=id;intro.public_key=pub;intro.nonce=std::string(32,'d');
    assert(roundtrip(intro).id==id);

    opal::RendezvousMessage offer;offer.type=opal::RendezvousType::Offer;offer.id=id;offer.session_id=std::string(32,'e');offer.public_key=pub;offer.host="2001:db8::1";offer.port=5555;offer.nonce=std::string(32,'f');
    auto offer2=roundtrip(offer);assert(offer2.host==offer.host&&offer2.port==5555&&offer2.session_id==offer.session_id);

    opal::RendezvousMessage ready=offer;ready.type=opal::RendezvousType::Ready;ready.host="203.0.113.9";ready.port=6000;
    assert(roundtrip(ready).port==6000);

    opal::RendezvousMessage relay;relay.type=opal::RendezvousType::RelayRequest;relay.session_id=std::string(32,'1');relay.public_key=pub;relay.nonce=std::string(32,'2');relay.signature=std::string(128,'3');
    assert(roundtrip(relay).session_id==relay.session_id);

    opal::RendezvousMessage relay_ready;relay_ready.type=opal::RendezvousType::RelayReady;relay_ready.session_id=std::string(32,'4');relay_ready.host="198.51.100.5";relay_ready.port=47993;relay_ready.allocation_id=std::string(32,'5');relay_ready.ttl_seconds=30;
    auto rr=roundtrip(relay_ready);assert(rr.allocation_id==relay_ready.allocation_id&&rr.ttl_seconds==30);

    opal::RendezvousMessage error;error.type=opal::RendezvousType::Error;error.error_code="HOST_OFFLINE";
    assert(roundtrip(error).error_code=="HOST_OFFLINE");

    opal::RendezvousMessage out;
    assert(!opal::parse_rendezvous_message("",out));
    assert(!opal::parse_rendezvous_message(std::string(opal::kRendezvousMaxMessageBytes+1,'X'),out));
    assert(!opal::parse_rendezvous_message("LEASE_HELLO bad bad",out));
    assert(!opal::parse_rendezvous_message("ERROR HOST OFFLINE",out));
    assert(!opal::parse_rendezvous_message("LEASE_OK "+id+" 0",out));
    return 0;
}
