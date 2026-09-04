#include <opal/crypto.hpp>
#include <opal/local_discovery.hpp>
#include <opal/peer_session.hpp>
#include <cassert>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
struct Identity{std::filesystem::path root,priv,pub;std::string public_hex;};
Identity identity(const char*name){Identity i;i.root=std::filesystem::temp_directory_path()/name;std::filesystem::remove_all(i.root);std::filesystem::create_directories(i.root);i.priv=i.root/"id.key";i.pub=i.root/"id.pub";assert(opal::ensure_identity(i.priv,i.pub));i.public_hex=opal::public_key_hex(i.pub);return i;}
}

int main(){
    static_assert(opal::kLocalDiscoveryReplyPort==47994);
    auto client_id=identity("opal-local-discovery-client"),host_id=identity("opal-local-discovery-host"),wrong_signer=identity("opal-local-discovery-wrong-signer");
    const auto rendezvous_id=opal::rendezvous_id_from_public_key(host_id.public_hex);assert(!rendezvous_id.empty());

    // A discovery request may not advertise a different peer port from the
    // UDP source port. Accepting that split recreates the fixed-reply-port ->
    // ephemeral-port race that fails on real LAN/Tailscale paths.
    {
        std::string mismatch_listener_error;auto mismatch_listener=opal::open_local_discovery_listener(0,"0.0.0.0",mismatch_listener_error);assert(mismatch_listener.fd>=0&&mismatch_listener.local_port>0);
        int fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);assert(fd>=0);sockaddr_in local{};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);assert(bind(fd,reinterpret_cast<sockaddr*>(&local),sizeof(local))==0);socklen_t local_len=sizeof(local);assert(getsockname(fd,reinterpret_cast<sockaddr*>(&local),&local_len)==0);const auto source_port=ntohs(local.sin_port);assert(source_port>0);
        sockaddr_in target{};target.sin_family=AF_INET;target.sin_port=htons(mismatch_listener.local_port);assert(inet_pton(AF_INET,"127.0.0.1",&target.sin_addr)==1);
        const auto advertised=static_cast<unsigned int>(source_port==65535?65534:source_port+1);const std::string request="OPAL_LOCAL_DISCOVER_V1 "+rendezvous_id+" "+client_id.public_hex+" "+std::string(32,'0')+" "+std::to_string(advertised);assert(sendto(fd,request.data(),request.size(),0,reinterpret_cast<sockaddr*>(&target),sizeof(target))==static_cast<ssize_t>(request.size()));
        opal::LocalDiscoveryHostResult mismatch_result;std::string mismatch_error;assert(!opal::wait_local_client(mismatch_listener,host_id.public_hex,host_id.priv,mismatch_result,120,mismatch_error));assert(mismatch_error=="local discovery timeout");close(fd);opal::close_udp_socket(mismatch_listener);
    }

    // A concrete IPv4 bind is the path used by the Tailscale host. It must
    // still produce a dual-stack socket because PeerSession normalizes IPv4
    // peers to IPv4-mapped AF_INET6 endpoints.
    std::string error;auto listener=opal::open_local_discovery_listener(0,"127.0.0.1",error);assert(listener.fd>=0&&listener.local_port>0);const auto listener_port=listener.local_port;
    sockaddr_storage listener_address{};socklen_t listener_length=sizeof(listener_address);assert(getsockname(listener.fd,reinterpret_cast<sockaddr*>(&listener_address),&listener_length)==0);assert(listener_address.ss_family==AF_INET6);

    opal::LocalDiscoveryHostResult host_result;std::string host_error;std::atomic<bool>host_found{false};
    std::thread discovery_host([&]{host_found.store(opal::wait_local_client(listener,host_id.public_hex,host_id.priv,host_result,2000,host_error));});
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    opal::LocalDiscoveryClientResult client_result;assert(opal::discover_local_host(rendezvous_id,client_id.public_hex,client_result,error,1000,"127.0.0.1",listener.local_port));
    discovery_host.join();assert(host_found.load());

    assert(client_result.rendezvous_id==rendezvous_id&&host_result.rendezvous_id==rendezvous_id);
    assert(client_result.host_public_key==host_id.public_hex&&host_result.client_public_key==client_id.public_hex);
    assert(client_result.session_id==host_result.session_id);
    assert(client_result.client_nonce==host_result.client_nonce);
    assert(client_result.host_nonce==host_result.host_nonce);
    assert(client_result.socket.fd>=0&&client_result.socket.local_port>0&&host_result.socket.fd>=0);
    sockaddr_storage client_socket_address{};socklen_t client_socket_length=sizeof(client_socket_address);assert(getsockname(client_result.socket.fd,reinterpret_cast<sockaddr*>(&client_socket_address),&client_socket_length)==0);assert(client_socket_address.ss_family==AF_INET6);
    sockaddr_storage host_socket_address{};socklen_t host_socket_length=sizeof(host_socket_address);assert(getsockname(host_result.socket.fd,reinterpret_cast<sockaddr*>(&host_socket_address),&host_socket_length)==0);assert(host_socket_address.ss_family==AF_INET6);
    assert(listener.fd>=0&&listener.local_port==listener_port);
    assert(host_result.socket.fd!=listener.fd&&host_result.socket.local_port==listener_port);
    assert(client_result.host.port==listener_port);
    assert(host_result.client.port==client_result.socket.local_port);

    opal::PeerHandshakeContext context;context.rendezvous_id=rendezvous_id;context.session_id=client_result.session_id;context.generation=1;context.client_identity=client_id.public_hex;context.host_identity=host_id.public_hex;context.client_nonce=client_result.client_nonce;context.host_nonce=host_result.host_nonce;context.auth_binding="pairing";
    opal::PeerSessionOptions host_options;host_options.client_side=false;host_options.socket=host_result.socket;host_options.peer=host_result.client;host_options.lan_peer=host_result.client;host_options.handshake=context;host_options.identity_private_key=host_id.priv;host_options.pairing_password="ABCD-EFGH-JKLM-NPQR";
    opal::PeerSessionOptions client_options;client_options.client_side=true;client_options.socket=client_result.socket;client_options.peer=client_result.host;client_options.lan_peer=client_result.host;client_options.handshake=context;client_options.identity_private_key=client_id.priv;client_options.pairing_password="ABCD-EFGH-JKLM-NPQR";

    opal::PeerSession host,client;std::string peer_host_error,peer_client_error;std::atomic<bool>host_started{false};
    std::thread peer_host([&]{host_started.store(host.start(std::move(host_options),peer_host_error));});
    std::this_thread::sleep_for(std::chrono::milliseconds(20));assert(client.start(std::move(client_options),peer_client_error));peer_host.join();assert(host_started.load());
    assert(client.established()&&host.established());assert(client.path_name()=="lan"&&host.path_name()=="lan");

    client.stop();host.stop();opal::close_udp_socket(listener);

    std::string bad_listener_error;auto bad_listener=opal::open_local_discovery_listener(0,"0.0.0.0",bad_listener_error);assert(bad_listener.fd>=0&&bad_listener.local_port>0);const auto bad_listener_port=bad_listener.local_port;
    opal::LocalDiscoveryHostResult bad_host_result;std::string bad_host_error;std::atomic<bool>bad_host_found{false};
    std::thread bad_discovery_host([&]{bad_host_found.store(opal::wait_local_client(bad_listener,host_id.public_hex,wrong_signer.priv,bad_host_result,2000,bad_host_error));});
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    opal::LocalDiscoveryClientResult rejected_result;std::string rejected_error;
    assert(!opal::discover_local_host(rendezvous_id,client_id.public_hex,rejected_result,rejected_error,300,"127.0.0.1",bad_listener.local_port));
    bad_discovery_host.join();assert(bad_host_found.load());assert(rejected_error=="local discovery offer signature invalid");
    assert(bad_listener.fd>=0&&bad_listener.local_port==bad_listener_port);
    assert(bad_host_result.socket.fd!=bad_listener.fd&&bad_host_result.socket.local_port==bad_listener_port);
    opal::close_udp_socket(bad_host_result.socket);opal::close_udp_socket(bad_listener);

    std::filesystem::remove_all(client_id.root);std::filesystem::remove_all(host_id.root);std::filesystem::remove_all(wrong_signer.root);return 0;
}