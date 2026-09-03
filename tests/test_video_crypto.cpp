#include <opal/video_crypto.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

int main(){
    opal::VideoKeys client_keys{},server_keys{};
    for(std::size_t i=0;i<client_keys.send_key.size();++i){
        client_keys.send_key[i]=static_cast<std::uint8_t>(i+1);
        client_keys.recv_key[i]=static_cast<std::uint8_t>(0x80+i);
    }
    for(std::size_t i=0;i<client_keys.send_nonce_base.size();++i){
        client_keys.send_nonce_base[i]=static_cast<std::uint8_t>(0x20+i);
        client_keys.recv_nonce_base[i]=static_cast<std::uint8_t>(0x40+i);
    }
    server_keys.send_key=client_keys.recv_key;
    server_keys.recv_key=client_keys.send_key;
    server_keys.send_nonce_base=client_keys.recv_nonce_base;
    server_keys.recv_nonce_base=client_keys.send_nonce_base;

    std::vector<std::uint8_t> plaintext(1000);for(std::size_t i=0;i<plaintext.size();++i)plaintext[i]=static_cast<std::uint8_t>(i);
    const std::vector<std::uint8_t> aad={'O','P','V','1'};std::vector<std::uint8_t> sealed,opened;
    assert(opal::seal_video_datagram(client_keys,42,aad,plaintext,sealed));
    assert(sealed.size()==plaintext.size()+16);
    assert(opal::open_video_datagram(server_keys,42,aad,sealed,opened)&&opened==plaintext);
    auto tampered=sealed;tampered[17]^=0x40;assert(!opal::open_video_datagram(server_keys,42,aad,tampered,opened));
    auto bad_aad=aad;bad_aad[0]^=1;assert(!opal::open_video_datagram(server_keys,42,bad_aad,sealed,opened));
    assert(opal::seal_video_datagram(server_keys,99,aad,plaintext,sealed));
    assert(opal::open_video_datagram(client_keys,99,aad,sealed,opened)&&opened==plaintext);

    opal::VideoCipher client_cipher(client_keys),server_cipher(server_keys);
    assert(client_cipher.valid()&&server_cipher.valid());
    std::array<std::uint8_t,1200> sealed_buffer{},opened_buffer{};

    std::size_t empty_sealed_size=0,empty_opened_size=0;
    const std::span<const std::uint8_t> empty_plaintext{};
    assert(client_cipher.seal(999,aad,empty_plaintext,sealed_buffer,empty_sealed_size));
    assert(empty_sealed_size==16);
    assert(server_cipher.open(999,aad,std::span<const std::uint8_t>(sealed_buffer.data(),empty_sealed_size),opened_buffer,empty_opened_size));
    assert(empty_opened_size==0);

    for(std::uint64_t sequence=1000;sequence<6000;++sequence){
        std::size_t sealed_size=0,opened_size=0;
        assert(client_cipher.seal(sequence,aad,plaintext,sealed_buffer,sealed_size));
        assert(sealed_size==plaintext.size()+16);
        assert(server_cipher.open(sequence,aad,std::span<const std::uint8_t>(sealed_buffer.data(),sealed_size),opened_buffer,opened_size));
        assert(opened_size==plaintext.size());
        assert(std::equal(plaintext.begin(),plaintext.end(),opened_buffer.begin()));
    }

    opal::ReplayWindow1024 replay;
    assert(replay.accept(100));assert(!replay.accept(100));
    assert(replay.accept(101));assert(replay.accept(99));assert(!replay.accept(99));
    replay.reset();assert(replay.accept(5000));assert(replay.accept(3977));assert(!replay.accept(3976));
    assert(replay.accept(7000));assert(!replay.accept(5000));
    return 0;
}
