#include <cassert>
#include <fstream>
#include <iterator>
#include <string>

static std::string read_all(const char*path){std::ifstream in(path);assert(in.good());return std::string(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());}
int main(){
    const auto header=read_all("include/opal/session.hpp");const auto session=read_all("src/session.cpp");const auto host=read_all("src/host.cpp");const auto client=read_all("src/client.cpp");
    assert(header.find("rendezvous_id")!=std::string::npos);assert(header.find("expected_host_public_key")!=std::string::npos);assert(header.find("control_token")==std::string::npos);assert(header.find("tunneled")==std::string::npos);
    assert(session.find("discover_local_host")!=std::string::npos);assert(session.find("local_discovery_enabled")!=std::string::npos);assert(session.find("RendezvousClient")!=std::string::npos);assert(session.find("host not found on LAN")!=std::string::npos);assert(session.find("PeerSession")!=std::string::npos);assert(session.find("start_native")!=std::string::npos);assert(session.find("generation")!=std::string::npos);assert(session.find("TunnelAccessHandle")==std::string::npos);assert(session.find("connect_tls")==std::string::npos);assert(session.find("tunnel_access")==std::string::npos);assert(session.find("zrok")==std::string::npos);
    assert(host.find("local_discovery_loop")!=std::string::npos);assert(host.find("register_host")!=std::string::npos);assert(host.find("wait_offer")!=std::string::npos);assert(host.find("PeerSession")!=std::string::npos);assert(host.find("start_native")!=std::string::npos);assert(host.find("tunnel_host")==std::string::npos);assert(host.find("server_tls_context")==std::string::npos);assert(host.find("listen_tcp")==std::string::npos);assert(host.find("zrok")==std::string::npos);
    assert(client.find("parse_connection_code")!=std::string::npos);assert(client.find("host_public_key")!=std::string::npos);assert(client.find("tunnel_connection_code")==std::string::npos);assert(client.find("zrok:")==std::string::npos);
    return 0;
}
