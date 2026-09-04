#include <opal/peer_handshake.hpp>
#include <opal/crypto.hpp>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace opal { namespace {

bool sha256(std::string_view input,std::array<std::uint8_t,32>&out){unsigned int n=0;return EVP_Digest(input.data(),input.size(),out.data(),&n,EVP_sha256(),nullptr)==1&&n==out.size();}

bool hmac(std::span<const std::uint8_t> key,std::span<const std::uint8_t> data,std::array<std::uint8_t,32>&out){unsigned int n=0;return HMAC(EVP_sha256(),key.data(),static_cast<int>(key.size()),data.data(),data.size(),out.data(),&n)!=nullptr&&n==out.size();}
bool hmac(std::span<const std::uint8_t> key,std::string_view data,std::array<std::uint8_t,32>&out){return hmac(key,std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data.data()),data.size()),out);}

bool hkdf_expand(std::span<const std::uint8_t> prk,std::string_view info,std::span<std::uint8_t> output){
    if(output.empty()||output.size()>255u*32u)return false;
    std::array<std::uint8_t,32> previous{};std::size_t previous_size=0,offset=0;std::uint8_t counter=1;
    while(offset<output.size()){
        std::vector<std::uint8_t> input;input.reserve(previous_size+info.size()+1);input.insert(input.end(),previous.begin(),previous.begin()+static_cast<std::ptrdiff_t>(previous_size));input.insert(input.end(),reinterpret_cast<const std::uint8_t*>(info.data()),reinterpret_cast<const std::uint8_t*>(info.data()+info.size()));input.push_back(counter++);
        if(!hmac(prk,input,previous)){OPENSSL_cleanse(previous.data(),previous.size());return false;}previous_size=previous.size();const auto count=std::min(previous.size(),output.size()-offset);std::memcpy(output.data()+offset,previous.data(),count);offset+=count;
    }
    OPENSSL_cleanse(previous.data(),previous.size());return true;
}

bool derive_shared(const PeerEphemeralKey&local,std::string_view peer_public_hex,std::array<std::uint8_t,32>&shared){
    if(!local.valid)return false;
    const auto peer=unhex(std::string(peer_public_hex));if(peer.size()!=32)return false;
    EVP_PKEY *local_key=EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519,nullptr,local.private_key.data(),local.private_key.size());EVP_PKEY *peer_key=EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519,nullptr,peer.data(),peer.size());EVP_PKEY_CTX *ctx=local_key?EVP_PKEY_CTX_new(local_key,nullptr):nullptr;size_t n=shared.size();const bool ok=ctx&&peer_key&&EVP_PKEY_derive_init(ctx)==1&&EVP_PKEY_derive_set_peer(ctx,peer_key)==1&&EVP_PKEY_derive(ctx,shared.data(),&n)==1&&n==shared.size();EVP_PKEY_CTX_free(ctx);EVP_PKEY_free(local_key);EVP_PKEY_free(peer_key);return ok;
}

bool derive_direction(std::span<const std::uint8_t>prk,const std::string&context,const char*channel,const char*direction,std::array<std::uint8_t,32>&key,std::array<std::uint8_t,12>&nonce){
    std::array<std::uint8_t,44>material{};const std::string info=std::string("OPAL-PEER-")+channel+"-"+direction+"-v1\n"+context;if(!hkdf_expand(prk,info,material)){OPENSSL_cleanse(material.data(),material.size());return false;}std::copy_n(material.begin(),32,key.begin());std::copy_n(material.begin()+32,12,nonce.begin());OPENSSL_cleanse(material.data(),material.size());return true;
}

bool derive_channel(std::span<const std::uint8_t>prk,const std::string&context,const char*channel,bool client_side,PeerChannelKeys&out){
    std::array<std::uint8_t,32>c2h_key{},h2c_key{};std::array<std::uint8_t,12>c2h_nonce{},h2c_nonce{};
    const bool ok=derive_direction(prk,context,channel,"c2h",c2h_key,c2h_nonce)&&derive_direction(prk,context,channel,"h2c",h2c_key,h2c_nonce);
    if(ok){if(client_side){out.send_key=c2h_key;out.send_nonce_base=c2h_nonce;out.recv_key=h2c_key;out.recv_nonce_base=h2c_nonce;}else{out.send_key=h2c_key;out.send_nonce_base=h2c_nonce;out.recv_key=c2h_key;out.recv_nonce_base=c2h_nonce;}}
    OPENSSL_cleanse(c2h_key.data(),c2h_key.size());OPENSSL_cleanse(h2c_key.data(),h2c_key.size());OPENSSL_cleanse(c2h_nonce.data(),c2h_nonce.size());OPENSSL_cleanse(h2c_nonce.data(),h2c_nonce.size());return ok;
}

}

bool generate_peer_ephemeral(PeerEphemeralKey&out){
    clear_peer_ephemeral(out);EVP_PKEY_CTX*ctx=EVP_PKEY_CTX_new_id(EVP_PKEY_X25519,nullptr);EVP_PKEY*key=nullptr;bool ok=ctx&&EVP_PKEY_keygen_init(ctx)==1&&EVP_PKEY_keygen(ctx,&key)==1;EVP_PKEY_CTX_free(ctx);if(!ok||!key){EVP_PKEY_free(key);return false;}size_t private_size=out.private_key.size(),public_size=out.public_key.size();ok=EVP_PKEY_get_raw_private_key(key,out.private_key.data(),&private_size)==1&&EVP_PKEY_get_raw_public_key(key,out.public_key.data(),&public_size)==1&&private_size==out.private_key.size()&&public_size==out.public_key.size();EVP_PKEY_free(key);out.valid=ok;if(!ok)clear_peer_ephemeral(out);return ok;
}

void clear_peer_ephemeral(PeerEphemeralKey&key){OPENSSL_cleanse(key.private_key.data(),key.private_key.size());OPENSSL_cleanse(key.public_key.data(),key.public_key.size());key.valid=false;}
std::string peer_ephemeral_public_hex(const PeerEphemeralKey&key){return key.valid?hex(key.public_key.data(),key.public_key.size()):std::string{};}

std::string peer_handshake_context(const PeerHandshakeContext&c){
    if(c.rendezvous_id.empty()||c.session_id.empty()||c.generation==0||c.client_identity.empty()||c.host_identity.empty()||c.client_nonce.empty()||c.host_nonce.empty()||c.auth_binding.empty())return {};
    return "OPAL-PEER-CONTEXT-v1\n"+c.rendezvous_id+"\n"+c.session_id+"\n"+std::to_string(c.generation)+"\n"+c.client_identity+"\n"+c.host_identity+"\n"+c.client_nonce+"\n"+c.host_nonce+"\n"+c.auth_binding;
}
std::string peer_client_hello_transcript(const PeerHandshakeContext&c,std::string_view client_ephemeral_public){const auto context=peer_handshake_context(c);if(context.empty()||client_ephemeral_public.size()!=64)return {};return "OPAL-PEER-CLIENT-HELLO-v1\n"+context+"\n"+std::string(client_ephemeral_public);}
std::string peer_host_welcome_transcript(const PeerHandshakeContext&c,std::string_view client_ephemeral_public,std::string_view host_ephemeral_public){const auto hello=peer_client_hello_transcript(c,client_ephemeral_public);if(hello.empty()||host_ephemeral_public.size()!=64)return {};return "OPAL-PEER-HOST-WELCOME-v1\n"+hello+"\n"+std::string(host_ephemeral_public);}
std::string peer_pairing_proof(std::string_view password,const PeerHandshakeContext&c,std::string_view client_ephemeral_public){if(password.empty())return {};const auto transcript=peer_client_hello_transcript(c,client_ephemeral_public);if(transcript.empty())return {};return hmac_sha256_hex(std::string(password),"OPAL-PEER-PAIRING-v1\n"+transcript);}

bool derive_peer_session_keys(const PeerHandshakeContext&c,const PeerEphemeralKey&local,std::string_view client_ephemeral_public,std::string_view host_ephemeral_public,bool client_side,PeerSessionKeys&out){
    clear_peer_session_keys(out);const auto context=peer_handshake_context(c);if(context.empty()||client_ephemeral_public.size()!=64||host_ephemeral_public.size()!=64)return false;const auto local_public=peer_ephemeral_public_hex(local);if(local_public.empty()||(client_side?local_public!=client_ephemeral_public:local_public!=host_ephemeral_public))return false;
    std::array<std::uint8_t,32>shared{},salt{},prk{};const auto peer_public=client_side?host_ephemeral_public:client_ephemeral_public;bool ok=derive_shared(local,peer_public,shared)&&sha256("OPAL-PEER-HKDF-SALT-v1\n"+context,salt)&&hmac(salt,shared,prk);
    if(ok)ok=derive_channel(prk,context,"CONTROL",client_side,out.control)&&derive_channel(prk,context,"MEDIA",client_side,out.media)&&derive_channel(prk,context,"PROBE",client_side,out.probe)&&derive_channel(prk,context,"RELAY",client_side,out.relay)&&hkdf_expand(prk,"OPAL-PEER-CONFIRM-v1\n"+context,out.confirmation_key);
    OPENSSL_cleanse(shared.data(),shared.size());OPENSSL_cleanse(salt.data(),salt.size());OPENSSL_cleanse(prk.data(),prk.size());if(!ok)clear_peer_session_keys(out);return ok;
}

std::string peer_confirmation_mac(const PeerSessionKeys&keys,std::string_view transcript){if(transcript.empty())return {};std::array<std::uint8_t,32>mac{};if(!hmac(keys.confirmation_key,"OPAL-PEER-FINISH-v1\n"+std::string(transcript),mac))return {};const auto result=hex(mac.data(),mac.size());OPENSSL_cleanse(mac.data(),mac.size());return result;}
void clear_peer_session_keys(PeerSessionKeys&keys){OPENSSL_cleanse(&keys,sizeof(keys));}

}
