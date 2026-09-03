#include <opal/crypto.hpp>
#include <opal/peer_session.hpp>
#include <opal/udp_transport.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
struct Identity{std::filesystem::path root,priv,pub;std::string public_hex;};
Identity identity(const char*name){Identity i;i.root=std::filesystem::temp_directory_path()/name;std::filesystem::remove_all(i.root);std::filesystem::create_directories(i.root);i.priv=i.root/"id.key";i.pub=i.root/"id.pub";assert(opal::ensure_identity(i.priv,i.pub));i.public_hex=opal::public_key_hex(i.pub);return i;}
}

int main(){
    auto client_id=identity("opal-peer-session-client"),host_id=identity("opal-peer-session-host");
    auto client_socket=opal::open_udp_socket(),host_socket=opal::open_udp_socket();assert(client_socket.fd>=0&&host_socket.fd>=0);
    opal::PeerHandshakeContext context;context.rendezvous_id="ABCD1234EFGH";context.session_id="00112233445566778899aabbccddeeff";context.generation=3;context.client_identity=client_id.public_hex;context.host_identity=host_id.public_hex;context.client_nonce="0102030405060708090a0b0c0d0e0f10";context.host_nonce="1112131415161718191a1b1c1d1e1f20";context.auth_binding="paired";

    std::mutex mu;std::vector<std::string>host_reliable,host_pointer;std::vector<std::uint8_t>host_media;
    opal::PeerSessionOptions host_options;host_options.client_side=false;host_options.socket=host_socket;host_options.peer={"::1",client_socket.local_port};host_options.handshake=context;host_options.identity_private_key=host_id.priv;host_options.reliable_input=[&](const std::string&s){std::lock_guard<std::mutex>l(mu);host_reliable.push_back(s);};host_options.pointer_input=[&](const std::string&s){std::lock_guard<std::mutex>l(mu);host_pointer.push_back(s);};host_options.media_datagram=[&](std::span<const std::uint8_t>b){std::lock_guard<std::mutex>l(mu);host_media.assign(b.begin(),b.end());};
    opal::PeerSessionOptions client_options;client_options.client_side=true;client_options.socket=client_socket;client_options.peer={"::1",host_socket.local_port};client_options.handshake=context;client_options.identity_private_key=client_id.priv;

    opal::PeerSession host,client;std::string host_error,client_error;std::atomic<bool>host_started{false};std::thread host_thread([&]{host_started.store(host.start(std::move(host_options),host_error));});std::this_thread::sleep_for(std::chrono::milliseconds(20));assert(client.start(std::move(client_options),client_error));host_thread.join();assert(host_started.load());assert(client.established()&&host.established());assert(client.generation()==3&&host.generation()==3);

    setenv("OPAL_TEST_DROP_FIRST_RELIABLE","1",1);assert(client.send_input("KEY 30 1"));unsetenv("OPAL_TEST_DROP_FIRST_RELIABLE");
    for(int i=0;i<20;++i)assert(client.send_pointer("POINTER "+std::to_string(i)+" "+std::to_string(i)));
    const std::vector<std::uint8_t>media={0xde,0xad,0xbe,0xef};assert(client.send_media_datagram(media));
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);for(;;){{std::lock_guard<std::mutex>l(mu);if(host_reliable.size()==1&&!host_pointer.empty()&&host_pointer.back()=="POINTER 19 19"&&host_media==media)break;}assert(std::chrono::steady_clock::now()<deadline);std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    {std::lock_guard<std::mutex>l(mu);assert(host_reliable.size()==1&&host_reliable[0]=="KEY 30 1");assert(host_pointer.back()=="POINTER 19 19");}
    const auto ack_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);while(client.reliable_pending()!=0&&std::chrono::steady_clock::now()<ack_deadline)std::this_thread::sleep_for(std::chrono::milliseconds(10));assert(client.reliable_pending()==0);assert(client.pointer_sequence()==20);

    client.stop();host.stop();assert(!client.running()&&!host.running());std::filesystem::remove_all(client_id.root);std::filesystem::remove_all(host_id.root);return 0;
}
