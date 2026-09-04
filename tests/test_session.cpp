#include <cassert>
#include <fstream>
#include <iterator>
#include <string>

static std::string read_all(const char*path){std::ifstream in(path);assert(in.good());return std::string(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());}
int main(){
    const auto header=read_all("include/opal/session.hpp");const auto session=read_all("src/session.cpp");const auto host=read_all("src/host.cpp");const auto client=read_all("src/client.cpp");
    assert(header.find("rendezvous_id")!=std::string::npos);assert(header.find("expected_host_public_key")!=std::string::npos);assert(header.find("control_token")==std::string::npos);assert(header.find("tunneled")==std::string::npos);
    assert(session.find("discover_local_host")!=std::string::npos);assert(session.find("local_discovery_enabled")!=std::string::npos);assert(session.find("RendezvousClient")!=std::string::npos);assert(session.find("host not found on LAN")!=std::string::npos);assert(session.find("PeerSession")!=std::string::npos);assert(session.find("start_native")!=std::string::npos);assert(session.find("generation")!=std::string::npos);assert(session.find("TunnelAccessHandle")==std::string::npos);assert(session.find("connect_tls")==std::string::npos);assert(session.find("tunnel_access")==std::string::npos);assert(session.find("zrok")==std::string::npos);
    const auto connect_begin=session.find("bool connect_generation");const auto healthy_begin=session.find("bool current_healthy",connect_begin);assert(connect_begin!=std::string::npos&&healthy_begin!=std::string::npos&&healthy_begin>connect_begin);const auto connect_source=session.substr(connect_begin,healthy_begin-connect_begin);assert(connect_source.find("pairing_password_provider()")==std::string::npos);
    const auto start_begin=session.find("bool start(){");const auto stop_begin=session.find("void stop(){",start_begin);assert(start_begin!=std::string::npos&&stop_begin!=std::string::npos&&stop_begin>start_begin);const auto start_source=session.substr(start_begin,stop_begin-start_begin);const auto provider_call=start_source.find("pairing_password_provider()");const auto connect_call=start_source.find("connect_generation(1,true)");assert(provider_call!=std::string::npos&&connect_call!=std::string::npos&&provider_call<connect_call);
    assert(host.find("local_discovery_loop")!=std::string::npos);assert(host.find("register_host")!=std::string::npos);assert(host.find("wait_offer")!=std::string::npos);assert(host.find("PeerSession")!=std::string::npos);assert(host.find("start_native")!=std::string::npos);assert(host.find("tunnel_host")==std::string::npos);assert(host.find("server_tls_context")==std::string::npos);assert(host.find("listen_tcp")==std::string::npos);assert(host.find("zrok")==std::string::npos);
    assert(client.find("parse_connection_code")!=std::string::npos);assert(client.find("host_public_key")!=std::string::npos);assert(client.find("tunnel_connection_code")==std::string::npos);assert(client.find("zrok:")==std::string::npos);
    return 0;
}
