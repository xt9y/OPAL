#include <opal/video_crypto.hpp>
#include <opal/net.hpp>
#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cassert>
#include <filesystem>
#include <thread>
#include <vector>
#include <unistd.h>

namespace fs=std::filesystem;

static std::uint16_t free_port(){
    int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);assert(fd>=0);
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=0;
    assert(bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0);
    socklen_t length=sizeof(address);assert(getsockname(fd,reinterpret_cast<sockaddr*>(&address),&length)==0);
    auto port=ntohs(address.sin_port);close(fd);return port;
}

int main(){
    auto root=fs::temp_directory_path()/"opal-video-crypto-test";fs::remove_all(root);fs::create_directories(root);
    auto cert=(root/"cert.pem").string();auto key=(root/"key.pem").string();assert(opal::ensure_tls_certificate(cert,key));
    SSL_CTX *server_ctx=opal::server_tls_context(cert,key);SSL_CTX *client_ctx=opal::client_tls_context();assert(server_ctx&&client_ctx);
    const std::string token="generation-token";const std::string client_pub="client-public-key";const std::string host_fp="host-fingerprint";
    const auto port=free_port();opal::VideoKeys server_keys{};
    std::thread server([&]{
        int listener=opal::listen_tcp(port,"127.0.0.1");assert(listener>=0);
        auto peer=opal::accept_tls(server_ctx,listener);assert(peer.ssl);
        assert(opal::derive_video_keys(peer.ssl,token,client_pub,host_fp,false,server_keys));
        opal::close_tls(peer);close(listener);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    auto client=opal::connect_tls_retry(client_ctx,"127.0.0.1",port,2000,20);assert(client.ssl);
    opal::VideoKeys client_keys{};assert(opal::derive_video_keys(client.ssl,token,client_pub,host_fp,true,client_keys));
    opal::close_tls(client);server.join();
    assert(client_keys.send_key==server_keys.recv_key);
    assert(client_keys.recv_key==server_keys.send_key);
    assert(client_keys.send_nonce_base==server_keys.recv_nonce_base);
    assert(client_keys.recv_nonce_base==server_keys.send_nonce_base);
    assert(!opal::derive_video_keys(nullptr,token,client_pub,host_fp,true,client_keys));

    std::vector<std::uint8_t> plaintext(1000);for(std::size_t i=0;i<plaintext.size();++i)plaintext[i]=static_cast<std::uint8_t>(i);
    const std::vector<std::uint8_t> aad={'O','P','V','1'};std::vector<std::uint8_t> sealed,opened;
    assert(opal::seal_video_datagram(client_keys,42,aad,plaintext,sealed));
    assert(sealed.size()==plaintext.size()+16);
    assert(opal::open_video_datagram(server_keys,42,aad,sealed,opened)&&opened==plaintext);
    auto tampered=sealed;tampered[17]^=0x40;assert(!opal::open_video_datagram(server_keys,42,aad,tampered,opened));
    auto bad_aad=aad;bad_aad[0]^=1;assert(!opal::open_video_datagram(server_keys,42,bad_aad,sealed,opened));
    assert(opal::seal_video_datagram(server_keys,99,aad,plaintext,sealed));
    assert(opal::open_video_datagram(client_keys,99,aad,sealed,opened)&&opened==plaintext);

    // The direct media hot path must reuse its EVP contexts and caller-owned
    // buffers across thousands of datagrams instead of allocating a cipher
    // context/vector for every ~1.1 KiB UDP packet.
    opal::VideoCipher client_cipher(client_keys),server_cipher(server_keys);
    assert(client_cipher.valid()&&server_cipher.valid());
    std::array<std::uint8_t,1200> sealed_buffer{},opened_buffer{};
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

    SSL_CTX_free(client_ctx);SSL_CTX_free(server_ctx);fs::remove_all(root);return 0;
}
