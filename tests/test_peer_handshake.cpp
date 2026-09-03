#include <opal/crypto.hpp>
#include <opal/peer_handshake.hpp>
#include <cassert>
#include <filesystem>
#include <string>

namespace {
struct Identity{std::filesystem::path root,priv,pub;std::string public_hex;};
Identity identity(const char*name){Identity i; i.root=std::filesystem::temp_directory_path()/name;std::filesystem::remove_all(i.root);std::filesystem::create_directories(i.root);i.priv=i.root/"id.key";i.pub=i.root/"id.pub";assert(opal::ensure_identity(i.priv,i.pub));i.public_hex=opal::public_key_hex(i.pub);assert(i.public_hex.size()==64);return i;}
bool all_zero(const auto &bytes){for(auto b:bytes)if(b!=0)return false;return true;}
}

int main(){
    auto client=identity("opal-peer-client");auto host=identity("opal-peer-host");
    opal::PeerHandshakeContext ctx;
    ctx.rendezvous_id="ABCD1234EFGH";ctx.session_id="00112233445566778899aabbccddeeff";ctx.generation=7;
    ctx.client_identity=client.public_hex;ctx.host_identity=host.public_hex;
    ctx.client_nonce="0102030405060708090a0b0c0d0e0f10";ctx.host_nonce="1112131415161718191a1b1c1d1e1f20";ctx.auth_binding="paired";

    opal::PeerEphemeralKey c_ephemeral,h_ephemeral;assert(opal::generate_peer_ephemeral(c_ephemeral));assert(opal::generate_peer_ephemeral(h_ephemeral));
    const auto c_pub=opal::peer_ephemeral_public_hex(c_ephemeral),h_pub=opal::peer_ephemeral_public_hex(h_ephemeral);assert(c_pub.size()==64&&h_pub.size()==64&&c_pub!=h_pub);
    const auto hello=opal::peer_client_hello_transcript(ctx,c_pub);const auto client_sig=opal::sign_hex(client.priv,hello);assert(!client_sig.empty()&&opal::verify_hex(client.public_hex,hello,client_sig));
    auto tampered=ctx;tampered.generation++;assert(!opal::verify_hex(client.public_hex,opal::peer_client_hello_transcript(tampered,c_pub),client_sig));
    const auto welcome=opal::peer_host_welcome_transcript(ctx,c_pub,h_pub);const auto host_sig=opal::sign_hex(host.priv,welcome);assert(!host_sig.empty()&&opal::verify_hex(host.public_hex,welcome,host_sig));
    assert(!opal::verify_hex(host.public_hex,opal::peer_host_welcome_transcript(ctx,c_pub,std::string(64,'0')),host_sig));

    opal::PeerSessionKeys ck,hk;assert(opal::derive_peer_session_keys(ctx,c_ephemeral,c_pub,h_pub,true,ck));assert(opal::derive_peer_session_keys(ctx,h_ephemeral,c_pub,h_pub,false,hk));
    auto mirror=[](const opal::PeerChannelKeys&a,const opal::PeerChannelKeys&b){assert(a.send_key==b.recv_key);assert(a.recv_key==b.send_key);assert(a.send_nonce_base==b.recv_nonce_base);assert(a.recv_nonce_base==b.send_nonce_base);};
    mirror(ck.control,hk.control);mirror(ck.media,hk.media);mirror(ck.probe,hk.probe);mirror(ck.relay,hk.relay);assert(ck.confirmation_key==hk.confirmation_key);
    const auto finish=opal::peer_confirmation_mac(ck,welcome);assert(!finish.empty()&&finish==opal::peer_confirmation_mac(hk,welcome));

    opal::PeerSessionKeys next;auto next_ctx=ctx;next_ctx.generation=8;assert(opal::derive_peer_session_keys(next_ctx,c_ephemeral,c_pub,h_pub,true,next));assert(next.media.send_key!=ck.media.send_key);
    opal::PeerSessionKeys changed_binding;auto binding_ctx=ctx;binding_ctx.auth_binding="pairing";assert(opal::derive_peer_session_keys(binding_ctx,c_ephemeral,c_pub,h_pub,true,changed_binding));assert(changed_binding.control.send_key!=ck.control.send_key);
    const auto proof1=opal::peer_pairing_proof("ABCD-EFGH",ctx,c_pub);const auto proof2=opal::peer_pairing_proof("WXYZ-EFGH",ctx,c_pub);assert(!proof1.empty()&&proof1!=proof2);

    opal::clear_peer_ephemeral(c_ephemeral);assert(!c_ephemeral.valid&&all_zero(c_ephemeral.private_key)&&all_zero(c_ephemeral.public_key));
    opal::clear_peer_session_keys(ck);assert(all_zero(ck.control.send_key)&&all_zero(ck.media.recv_key)&&all_zero(ck.confirmation_key));
    opal::clear_peer_session_keys(hk);opal::clear_peer_session_keys(next);opal::clear_peer_session_keys(changed_binding);opal::clear_peer_ephemeral(h_ephemeral);
    std::filesystem::remove_all(client.root);std::filesystem::remove_all(host.root);return 0;
}
