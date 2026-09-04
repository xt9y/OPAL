#include <cassert>
#include <fstream>
#include <iterator>
#include <string>

static std::string read_all(const char*path){std::ifstream in(path);assert(in.good());return std::string(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());}

int main(){
    const auto host=read_all("src/host.cpp");
    const auto begin=host.find("void local_discovery_loop");
    const auto end=host.find("int native_host_loop",begin);
    assert(begin!=std::string::npos&&end!=std::string::npos&&end>begin);
    const auto source=host.substr(begin,end-begin);
    const auto loop=source.find("for(;;)");
    const auto tailnet=source.find("local_tailnet_ipv4()");
    assert(loop!=std::string::npos&&tailnet!=std::string::npos&&loop<tailnet);
    assert(source.find("tailscale0 has no IPv4 address\\n\";return;")==std::string::npos);
    assert(source.find("close_udp_socket(listener)")!=std::string::npos);
    assert(source.find("std::this_thread::sleep_for(std::chrono::seconds(1))")!=std::string::npos);
    return 0;
}
