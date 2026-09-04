#include <cassert>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

static std::string read_all(const char *path){std::ifstream f(path);assert(f.good());return std::string((std::istreambuf_iterator<char>(f)),{});}

int main() {
    std::ifstream f("systemd/opal-host.service");
    assert(f.good());
    std::stringstream ss;ss<<f.rdbuf();const auto unit=ss.str();
    assert(unit.find("ExecStart=/usr/local/bin/opal --internal-host-daemon")!=std::string::npos);
    assert(unit.find("Restart=always")!=std::string::npos);
    assert(unit.find("Restart=on-failure")==std::string::npos);

    const auto host=read_all("src/host.cpp");
    assert(host.find("LAN / direct UDP / encrypted relay")!=std::string::npos);
    assert(host.find("local_discovery_loop")!=std::string::npos);
    assert(host.find("open_local_discovery_listener")!=std::string::npos);
    assert(host.find("LAN discovery remains active")!=std::string::npos);
    assert(host.find("register_host")!=std::string::npos);
    assert(host.find("wait_offer")!=std::string::npos);
    assert(host.find("PeerSession")!=std::string::npos);
    assert(host.find("47991")==std::string::npos);
    assert(host.find("zrok")==std::string::npos);
    assert(host.find("server_tls_context")==std::string::npos);
    return 0;
}
