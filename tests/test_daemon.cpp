#include <cassert>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

static std::string read_all(const char *path){std::ifstream f(path);assert(f.good());return std::string((std::istreambuf_iterator<char>(f)),{});}

int main() {
    std::ifstream f("system/opal-host.service");
    assert(f.good());
    std::stringstream ss;ss<<f.rdbuf();const auto unit=ss.str();
    assert(unit.find("ExecStart=/usr/local/bin/opal --internal-host-daemon")!=std::string::npos);
    assert(unit.find("Restart=always")!=std::string::npos);
    assert(unit.find("Restart=on-failure")==std::string::npos);
    assert(unit.find("RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6 AF_NETLINK")!=std::string::npos);

    const auto host=read_all("src/host.cpp");
    assert(host.find("Tailscale direct / direct UDP / encrypted relay")!=std::string::npos);
    assert(host.find("local_discovery_loop")!=std::string::npos);
    assert(host.find("local_tailnet_ipv4")!=std::string::npos);
    assert(host.find("open_local_discovery_listener")!=std::string::npos);
    assert(host.find("Tailscale discovery remains active")!=std::string::npos);
    assert(host.find("register_host")!=std::string::npos);
    assert(host.find("wait_offer")!=std::string::npos);
    assert(host.find("PeerSession")!=std::string::npos);
    assert(host.find("47991")==std::string::npos);
    assert(host.find("zrok")==std::string::npos);
    assert(host.find("server_tls_context")==std::string::npos);

    const auto media_ready=host.find("if(parse_media_ready(line,media_generation,stream,debug))");
    const auto media_feedback=host.find("if(sender_started.load()&&sender_ptr->handle_control_line(line))",media_ready);
    assert(media_ready!=std::string::npos&&media_feedback!=std::string::npos&&media_feedback>media_ready);
    const auto media_start=host.substr(media_ready,media_feedback-media_ready);
    assert(media_start.find("sender_starting.exchange(true)")!=std::string::npos);
    assert(media_start.find("sender_start_thread=std::thread")!=std::string::npos);
    assert(media_start.find("start_native")!=std::string::npos);
    assert(host.find("if(sender_start_thread.joinable())sender_start_thread.join();sender.stop()")!=std::string::npos);
    return 0;
}
