#include <cassert>
#include <fstream>
#include <sstream>
#include <string>

static std::string read_all(const char* path)
{
    std::ifstream in(path);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

int main()
{
    const auto source = read_all("src/platform/windows/tailnet.cpp");
    assert(source.find("ProgramFiles") != std::string::npos);
    assert(source.find("tailscale.exe") != std::string::npos);
    assert(source.find("_popen") != std::string::npos);
    assert(source.find("_pclose") != std::string::npos);
    assert(source.find("ip -4") != std::string::npos);
    assert(source.find("status --peers=true --self=false") != std::string::npos);
    assert(source.find("parse_tailnet_status_ipv4s") != std::string::npos);
    assert(source.find("/Applications/Tailscale.app") == std::string::npos);
    return 0;
}
