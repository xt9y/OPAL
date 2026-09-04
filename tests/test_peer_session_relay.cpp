#include <opal/crypto.hpp>
#include <opal/peer_session.hpp>
#include <opal/relay_protocol.hpp>
#include <opal/udp_transport.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
struct Identity{std::filesystem::path root,priv,pub;std::string public_hex;};
Identity identity(const char*name){Identity i;i.root=std::filesystem::temp_directory_path()/name;std::filesystem::remove_all(i.root);std::filesystem::create_directories(i.root);i.priv=i.root/"id.key";i.pub=i.root/"id.pub";assert(opal::ensure_identity(i.priv,i.pub));i.public_hex=opal::public_key_hex(i.pub);assert(!i.public_hex.empty());return i;}
struct Side{bool seen=false;sockaddr_storage address{};socklen_t length=0;std::vector<std::uint8_t>pending;};
}

int main(){
    auto client_id=identity("opal-peer-relay-client"),host_id=identity("opal-peer-relay-host");
    auto client_socket=opal::open_udp_socket(),host_socket=opal::open_udp_socket(),relay_socket=opal::open_udp_socket();
    assert(client_socket.fd>=0&&host_socket.fd>=0&&relay_socket.fd>=0);
    const std::string allocation="00112233445566778899aabbccddeeff";

    std::atomic<bool>relay_run{true};
    std::thread relay_thread([&]{
        Side client_side,host_side;std::vector<std::uint8_t>buffer(opal::kRelayHeaderBytes+opal::kRelayMaxInnerBytes);
        while(relay_run.load()){
            sockaddr_storage source{};socklen_t source_len=sizeof(source);const int n=opal::recv_datagram(relay_socket.fd,buffer,source,source_len,20);if(n<=0)continue;
            opal::RelayEnvelope envelope;if(!opal::parse_relay_datagram(std::span<const std::uint8_t>(buffer.data(),static_cast<std::size_t>(n)),envelope)||envelope.allocation_id!=allocation)continue;
            Side&self=envelope.role==opal::RelayRole::Client?client_side:host_side;Side&other=envelope.role==opal::RelayRole::Client?host_side:client_side;
            self.seen=true;self.address=source;self.length=source_len;
            if(other.seen)assert(opal::send_datagram(relay_socket.fd,other.address,other.length,envelope.inner));
            else self.pending.assign(envelope.inner.begin(),envelope.inner.end());
            if(self.seen&&other.seen&&!other.pending.empty()){assert(opal::send_datagram(relay_socket.fd,self.address,self.length,other.pending));other.pending.clear();}
        }
    });

    opal::PeerHandshakeContext context;context.rendezvous_id="ABCD1234EFGH";context.session_id="102132435465768798a9babbdcddedef";context.generation=4;context.client_identity=client_id.public_hex;context.host_identity=host_id.public_hex;context.client_nonce="0102030405060708090a0b0c0d0e0f10";context.host_nonce="1112131415161718191a1b1c1d1e1f20";context.auth_binding="paired";
    const opal::RendezvousEndpoint relay_endpoint{"::1",relay_socket.local_port};

    std::mutex mu;std::vector<std::string>host_reliable,host_pointer;std::vector<std::uint8_t>host_media;
    opal::PeerSessionOptions host_options;host_options.client_side=false;host_options.socket=host_socket;host_options.peer={"198.51.100.20",49001};host_options.relay=opal::PeerRelayFallback{relay_endpoint,allocation,opal::RelayRole::Host};host_options.direct_handshake_timeout_ms=80;host_options.relay_handshake_timeout_ms=2500;host_options.handshake=context;host_options.identity_private_key=host_id.priv;host_options.reliable_input=[&](const std::string&s){std::lock_guard<std::mutex>l(mu);host_reliable.push_back(s);};host_options.pointer_input=[&](const std::string&s){std::lock_guard<std::mutex>l(mu);host_pointer.push_back(s);};host_options.media_datagram=[&](std::span<const std::uint8_t>b){std::lock_guard<std::mutex>l(mu);host_media.assign(b.begin(),b.end());};
    opal::PeerSessionOptions client_options;client_options.client_side=true;client_options.socket=client_socket;client_options.peer={"203.0.113.20",49002};client_options.relay=opal::PeerRelayFallback{relay_endpoint,allocation,opal::RelayRole::Client};client_options.direct_handshake_timeout_ms=80;client_options.relay_handshake_timeout_ms=2500;client_options.handshake=context;client_options.identity_private_key=client_id.priv;

    opal::PeerSession host,client;std::string host_error,client_error;std::atomic<bool>host_started{false};
    std::thread host_thread([&]{host_started.store(host.start(std::move(host_options),host_error));});
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(client.start(std::move(client_options),client_error));host_thread.join();assert(host_started.load());
    assert(client.established()&&host.established());assert(client.path_name()=="relay"&&host.path_name()=="relay");

    assert(client.send_input("KEY 30 1"));for(int i=0;i<8;++i)assert(client.send_pointer("POINTER "+std::to_string(i)+" "+std::to_string(i)));
    const std::vector<std::uint8_t>media={0xca,0xfe,0xba,0xbe};assert(client.send_media_datagram(media));
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    for(;;){{std::lock_guard<std::mutex>l(mu);if(host_reliable.size()==1&&!host_pointer.empty()&&host_pointer.back()=="POINTER 7 7"&&host_media==media)break;}assert(std::chrono::steady_clock::now()<deadline);std::this_thread::sleep_for(std::chrono::milliseconds(10));}

    client.stop();host.stop();relay_run.store(false);relay_thread.join();opal::close_udp_socket(relay_socket);
    std::filesystem::remove_all(client_id.root);std::filesystem::remove_all(host_id.root);return 0;
}
