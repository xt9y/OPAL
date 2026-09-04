#include <opal/crypto.hpp>
#include <cassert>
#include <fstream>
#include <iterator>
#include <string>

static std::string read_all(const char*path){std::ifstream in(path);assert(in.good());return std::string((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());}
static void no_legacy_networking(const std::string&text){assert(text.find("zrok2")==std::string::npos);assert(text.find("tunnel_access")==std::string::npos);assert(text.find("tunnel_host_start")==std::string::npos);assert(text.find("TunnelAccessHandle")==std::string::npos);assert(text.find("connect_tls")==std::string::npos);assert(text.find("server_tls_context")==std::string::npos);}
int main(){
    const auto code=opal::pairing_code();assert(code.size()==19&&code[4]=='-'&&code[9]=='-'&&code[14]=='-');
    const auto input=read_all("src/input_helper.cpp");assert(input.find("O_CLOEXEC")!=std::string::npos&&input.find("O_NONBLOCK")==std::string::npos);
    const auto wake=read_all("src/wake.cpp");assert(wake.find("SO_RCVTIMEO")!=std::string::npos&&wake.find("SO_SNDTIMEO")!=std::string::npos&&wake.find("secret.empty()")!=std::string::npos);
    const auto client=read_all("src/client.cpp"),host=read_all("src/host.cpp"),session=read_all("src/session.cpp"),setup=read_all("src/setup.cpp"),system=read_all("src/system.cpp"),makefile=read_all("Makefile");
    no_legacy_networking(client);no_legacy_networking(host);no_legacy_networking(session);no_legacy_networking(setup);no_legacy_networking(system);no_legacy_networking(makefile);
    assert(client.find("parse_connection_code")!=std::string::npos&&client.find("host_public_key")!=std::string::npos);
    assert(host.find("RendezvousClient")!=std::string::npos&&host.find("register_host")!=std::string::npos&&host.find("PeerSession")!=std::string::npos&&host.find("start_native")!=std::string::npos);
    assert(session.find("RendezvousClient")!=std::string::npos&&session.find("PeerSession")!=std::string::npos&&session.find("start_native")!=std::string::npos);
    const auto peer=read_all("src/peer_session.cpp");assert(peer.find("lan_handshake_timeout_ms")!=std::string::npos&&peer.find("direct_handshake_timeout_ms")!=std::string::npos&&peer.find("relay_handshake_timeout_ms")!=std::string::npos&&peer.find("wrap_relay_datagram")!=std::string::npos&&peer.find("path=\"lan\"")!=std::string::npos&&peer.find("path=\"direct\"")!=std::string::npos&&peer.find("path=\"relay\"")!=std::string::npos);
    const auto handshake=read_all("src/peer_handshake.cpp");assert(handshake.find("X25519")!=std::string::npos&&handshake.find("HKDF")!=std::string::npos&&handshake.find("OPENSSL_cleanse")!=std::string::npos);
    const auto relay=read_all("server/rendezvous_server.cpp");assert(relay.find("parse_relay_datagram")!=std::string::npos&&relay.find("relay.inner.data()")!=std::string::npos);assert(relay.find("open_video_datagram")==std::string::npos&&relay.find("VideoCipher")==std::string::npos);
    const auto udp=read_all("src/udp_transport.cpp");assert(udp.find("STUN")==std::string::npos&&udp.find("stun.")==std::string::npos&&udp.find("IFF_LOOPBACK")!=std::string::npos&&udp.find("IFF_POINTOPOINT")!=std::string::npos);
    const auto media=read_all("src/media.cpp");assert(media.find("-cursor no")!=std::string::npos&&media.find("-draw_mouse 0")!=std::string::npos);
    const auto packet_header=read_all("include/opal/video_packet.hpp");assert(packet_header.find("Keepalive=6")!=std::string::npos);
    const auto service=read_all("systemd/opal-host.service");assert(service.find("NoNewPrivileges=true")!=std::string::npos&&service.find("PrivateTmp=true")!=std::string::npos&&service.find("RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6")!=std::string::npos&&service.find("UMask=0077")!=std::string::npos);
    const auto readme=read_all("README");no_legacy_networking(readme);assert(readme.find("Performance-first Linux remote desktop")!=std::string::npos);
    assert(makefile.find("rendezvous-server")!=std::string::npos&&makefile.find("test-peer-session")!=std::string::npos&&makefile.find("test-peer-session-relay")!=std::string::npos&&makefile.find("test-video-receiver-architecture")!=std::string::npos&&makefile.find("test-rendezvous-server")!=std::string::npos);assert(makefile.find("src/net.cpp")==std::string::npos&&makefile.find("direct_video_session.cpp")==std::string::npos&&makefile.find("-lssl")==std::string::npos);
    const auto ci=read_all(".github/workflows/ci.yml");assert(ci.find("workflow_dispatch")!=std::string::npos);assert(ci.find("\n  push:")==std::string::npos);assert(ci.find("test-net")==std::string::npos&&ci.find("test-tunnel-recovery")==std::string::npos&&ci.find("test-direct-video-session")==std::string::npos);assert(ci.find("test-peer-session-relay")!=std::string::npos&&ci.find("sanitize")!=std::string::npos&&ci.find("stress")!=std::string::npos);
    return 0;
}
