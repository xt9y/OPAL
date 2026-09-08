#include <opal/tailnet.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

namespace opal {
namespace {

std::string shell_quote(std::string_view value)
{
    std::string out = "'";
    for (const char c : value) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += '\'';
    return out;
}

std::string read_command_text(const std::string& command)
{
    if (command.empty()) return {};
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return {};
    std::array<char,512> buffer{};
    std::string text;
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) text += buffer.data();
    (void)pclose(pipe);
    return text;
}

std::string first_word(const std::string& text)
{
    std::istringstream values(text);
    std::string value;
    values >> value;
    return value;
}

std::string tailscale_cli_path()
{
    if (const char* configured = std::getenv("OPAL_TAILSCALE_CLI"); configured && *configured && access(configured, X_OK) == 0)
        return configured;

    const auto from_path = first_word(read_command_text("command -v tailscale 2>/dev/null"));
    if (!from_path.empty() && access(from_path.c_str(), X_OK) == 0) return from_path;

    for (const char* candidate : {
             "/usr/local/bin/tailscale",
             "/opt/homebrew/bin/tailscale",
             "/Applications/Tailscale.app/Contents/MacOS/Tailscale"}) {
        if (access(candidate, X_OK) == 0) return candidate;
    }

    if (const char* home = std::getenv("HOME"); home && *home) {
        const std::string candidate = std::string(home) + "/Applications/Tailscale.app/Contents/MacOS/Tailscale";
        if (access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    return {};
}

std::string tailscale_command(std::string_view arguments)
{
    const auto executable = tailscale_cli_path();
    if (executable.empty()) return {};
    return "TAILSCALE_BE_CLI=1 " + shell_quote(executable) + " " + std::string(arguments) + " 2>/dev/null";
}

std::string first_tailnet_ipv4(const std::string& text)
{
    std::istringstream values(text);
    std::string value;
    while (values >> value) if (is_tailnet_ipv4(value)) return value;
    return {};
}

}

bool tailscale_cli_available()
{
    return !tailscale_cli_path().empty();
}

std::vector<std::string> tailnet_peer_ipv4s()
{
    const auto command = tailscale_command("status --peers=true --self=false");
    return command.empty() ? std::vector<std::string>{} : parse_tailnet_status_ipv4s(read_command_text(command));
}

std::string local_tailnet_ipv4()
{
    const auto command = tailscale_command("ip -4");
    return command.empty() ? std::string{} : first_tailnet_ipv4(read_command_text(command));
}

}
