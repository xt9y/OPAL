#include <opal/crypto.hpp>
#include <opal/relay_protocol.hpp>
#include <opal/rendezvous_protocol.hpp>
#include <opal/rendezvous_server.hpp>
#include <cassert>
#include <filesystem>
#include <string>

namespace {
struct Identity {std::filesystem::path priv,pub;std::string public_hex,id;};
Identity make_identity(const std::string&name){auto root=std::filesystem::temp_directory_path()/name;std::filesystem::remove_all(root);std::filesystem::create_directories(root);Identity i{root/"id.key",root/"id.pub",{}, {}};assert(opal::ensure_identity(i.priv,i.pub));i.public_hex=opal::public_key_hex(i.pub);i.id=opal::rendezvous_id_from_public_key(i.public_hex);assert(!i.public_hex.empty()&&!i.id.empty());return i;}
void cleanup(const Identity&i){std::filesystem::remove_all(i.priv.parent_path());}
}

int main(){
    auto host=make_identity("opal-rendezvous-host-test");auto client=make_identity("opal-rendezvous-client-test");
    opal::RendezvousEndpoint host_ep{"198.51.100.10",41000},client_ep{"203.0.113.20",42000},attacker_ep{"203.0.113.99",43000},relay_ep{"192.0.2.7",47992};
    opal::RendezvousServerState state(relay_ep);

    opal::RendezvousMessage hello;hello.type=opal::RendezvousType::LeaseHello;hello.id=host.id;hello.public_key=host.public_hex;
    auto out=state.process(hello,host_ep,1000);assert(out.size()==1&&out[0].target==host_ep&&out[0].message.type==opal::RendezvousType::LeaseChallenge);const auto challenge=out[0].message.nonce;assert(challenge.size()==32);
    opal::RendezvousMessage bad_proof;bad_proof.type=opal::RendezvousType::LeaseProof;bad_proof.id=host.id;bad_proof.public_key=host.public_hex;bad_proof.nonce=challenge;bad_proof.signature=std::string(128,'0');out=state.process(bad_proof,host_ep,1100);assert(out.size()==1&&out[0].message.type==opal::RendezvousType::Error&&out[0].message.error_code=="AUTH_FAILED");
    out=state.process(hello,host_ep,1200);assert(out.size()==1);const auto challenge2=out[0].message.nonce;opal::RendezvousMessage proof;proof.type=opal::RendezvousType::LeaseProof;proof.id=host.id;proof.public_key=host.public_hex;proof.nonce=challenge2;proof.signature=opal::sign_hex(host.priv,opal::rendezvous_lease_transcript(host.id,host.public_hex,challenge2));assert(!proof.signature.empty());out=state.process(proof,host_ep,1300);assert(out.size()==1&&out[0].message.type==opal::RendezvousType::LeaseOk&&out[0].message.ttl_seconds==45);assert(state.active_leases(1300)==1);

    opal::RendezvousMessage intro;intro.type=opal::RendezvousType::Introduce;intro.id=host.id;intro.public_key=client.public_hex;intro.nonce=opal::random_hex(16);out=state.process(intro,client_ep,1400);assert(out.size()==1&&out[0].target==host_ep&&out[0].message.type==opal::RendezvousType::Offer);const auto offer=out[0].message;assert(offer.host==client_ep.host&&offer.port==client_ep.port&&offer.public_key==client.public_hex&&offer.nonce==intro.nonce);assert(state.pending_introductions(1400)==1);

    opal::RendezvousMessage attacker_accept;attacker_accept.type=opal::RendezvousType::Accept;attacker_accept.id=host.id;attacker_accept.session_id=offer.session_id;attacker_accept.public_key=host.public_hex;attacker_accept.nonce=opal::random_hex(16);attacker_accept.signature=opal::sign_hex(host.priv,opal::rendezvous_accept_transcript(host.id,offer.session_id,client.public_hex,intro.nonce,attacker_accept.nonce));out=state.process(attacker_accept,attacker_ep,1450);assert(out.size()==1&&out[0].target==attacker_ep&&out[0].message.type==opal::RendezvousType::Error);
    opal::RendezvousMessage accept=attacker_accept;accept.nonce=opal::random_hex(16);accept.signature=opal::sign_hex(host.priv,opal::rendezvous_accept_transcript(host.id,offer.session_id,client.public_hex,intro.nonce,accept.nonce));out=state.process(accept,host_ep,1500);assert(out.size()==1&&out[0].target==client_ep&&out[0].message.type==opal::RendezvousType::Ready);const auto ready=out[0].message;assert(ready.id==host.id&&ready.session_id==offer.session_id&&ready.public_key==host.public_hex&&ready.host==host_ep.host&&ready.port==host_ep.port&&ready.nonce==accept.nonce);

    // Relay allocation is bound to the accepted introduction and both signed peer identities.
    opal::RendezvousMessage client_relay;client_relay.type=opal::RendezvousType::RelayRequest;client_relay.session_id=offer.session_id;client_relay.public_key=client.public_hex;client_relay.nonce=opal::random_hex(16);client_relay.signature=opal::sign_hex(client.priv,opal::relay_request_transcript(client_relay.session_id,client_relay.public_key,client_relay.nonce));
    out=state.process(client_relay,client_ep,1600);assert(out.size()==1&&out[0].message.type==opal::RendezvousType::RelayReady);const auto allocation=out[0].message.allocation_id;assert(out[0].message.host==relay_ep.host&&out[0].message.port==relay_ep.port&&allocation.size()==32);
    opal::RendezvousEndpoint target;assert(!state.relay_target(allocation,opal::RelayRole::Client,client_ep,1600,target));
    auto attacker_relay=client_relay;attacker_relay.nonce=opal::random_hex(16);attacker_relay.signature=opal::sign_hex(client.priv,opal::relay_request_transcript(attacker_relay.session_id,attacker_relay.public_key,attacker_relay.nonce));out=state.process(attacker_relay,attacker_ep,1650);assert(out.size()==1&&out[0].message.type==opal::RendezvousType::Error&&out[0].message.error_code=="AUTH_FAILED");
    opal::RendezvousMessage host_relay;host_relay.type=opal::RendezvousType::RelayRequest;host_relay.session_id=offer.session_id;host_relay.public_key=host.public_hex;host_relay.nonce=opal::random_hex(16);host_relay.signature=opal::sign_hex(host.priv,opal::relay_request_transcript(host_relay.session_id,host_relay.public_key,host_relay.nonce));out=state.process(host_relay,host_ep,1700);assert(out.size()==1&&out[0].message.type==opal::RendezvousType::RelayReady&&out[0].message.allocation_id==allocation);
    assert(state.relay_target(allocation,opal::RelayRole::Client,client_ep,1700,target)&&target==host_ep);assert(state.relay_target(allocation,opal::RelayRole::Host,host_ep,1700,target)&&target==client_ep);assert(!state.relay_target(allocation,opal::RelayRole::Client,attacker_ep,1700,target));assert(!state.relay_target(allocation,opal::RelayRole::Host,client_ep,1700,target));assert(!state.relay_target(allocation,opal::RelayRole::Client,client_ep,40000,target));

    opal::RendezvousMessage expired_intro=intro;expired_intro.nonce=opal::random_hex(16);out=state.process(expired_intro,client_ep,47000);assert(out.size()==1&&out[0].message.type==opal::RendezvousType::Error&&out[0].message.error_code=="HOST_OFFLINE");assert(state.active_leases(47000)==0);
    auto unknown=client.id;opal::RendezvousMessage offline=intro;offline.id=unknown;out=state.process(offline,client_ep,48000);assert(out.size()==1&&out[0].target==client_ep&&out[0].message.error_code=="HOST_OFFLINE");

    cleanup(host);cleanup(client);return 0;
}
