#include <opal/tailnet.hpp>

#include <array>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace opal {
namespace {

std::string read_command_text(const char* command)
{
    if (!command || !*command) return {};
    FILE* pipe = popen(command, "r");
    if (!pipe) return {};
    std::array<char,512> buffer{};
    std::string text;
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) text += buffer.data();
    (void)pclose(pipe);
    return text;
}

std::string first_tailnet_ipv4(const std::string& text)
{
    std::istringstream values(text);
    std::string value;
    while (values >> value) if (is_tailnet_ipv4(value)) return value;
    return {};
}

}

std::vector<std::string> tailnet_peer_ipv4s()
{
    return parse_tailnet_status_ipv4s(
        read_command_text("tailscale status --peers=true --self=false 2>/dev/null"));
}

std::string local_tailnet_ipv4()
{
    return first_tailnet_ipv4(read_command_text("tailscale ip -4 2>/dev/null"));
}

}
