#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace opal {

struct PeerHandshakeContext {
    std::string rendezvous_id;
    std::string session_id;
    std::uint32_t generation=0;
    std::string client_identity;
    std::string host_identity;
    std::string client_nonce;
    std::string host_nonce;
    std::string auth_binding;
};

struct PeerEphemeralKey {
    std::array<std::uint8_t,32> private_key{};
    std::array<std::uint8_t,32> public_key{};
    bool valid=false;
};

struct PeerChannelKeys {
    std::array<std::uint8_t,32> send_key{},recv_key{};
    std::array<std::uint8_t,12> send_nonce_base{},recv_nonce_base{};
};

struct PeerSessionKeys {
    PeerChannelKeys control;
    PeerChannelKeys media;
    PeerChannelKeys probe;
    PeerChannelKeys relay;
    std::array<std::uint8_t,32> confirmation_key{};
};

bool generate_peer_ephemeral(PeerEphemeralKey&);
void clear_peer_ephemeral(PeerEphemeralKey&);
std::string peer_ephemeral_public_hex(const PeerEphemeralKey&);
std::string peer_handshake_context(const PeerHandshakeContext&);
std::string peer_client_hello_transcript(const PeerHandshakeContext&,std::string_view client_ephemeral_public);
std::string peer_host_welcome_transcript(const PeerHandshakeContext&,std::string_view client_ephemeral_public,
                                         std::string_view host_ephemeral_public);
std::string peer_pairing_proof(std::string_view pairing_password,const PeerHandshakeContext&,
                               std::string_view client_ephemeral_public);
bool derive_peer_session_keys(const PeerHandshakeContext&,const PeerEphemeralKey &local_ephemeral,
                              std::string_view client_ephemeral_public,std::string_view host_ephemeral_public,
                              bool client_side,PeerSessionKeys&);
std::string peer_confirmation_mac(const PeerSessionKeys&,std::string_view transcript);
void clear_peer_session_keys(PeerSessionKeys&);

}
