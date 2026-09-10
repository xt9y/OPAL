#include <opal/tailnet.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
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

std::string first_tailnet_ipv4(const std::string& text)
{
    std::istringstream values(text);
    std::string value;
    while (values >> value) if (is_tailnet_ipv4(value)) return value;
    return {};
}

void add_candidate(std::vector<std::string>& candidates, std::string path)
{
    if (path.empty() || access(path.c_str(), X_OK) != 0) return;
    if (std::find(candidates.begin(), candidates.end(), path) == candidates.end())
        candidates.push_back(std::move(path));
}

std::vector<std::string> tailscale_cli_candidates()
{
    std::vector<std::string> candidates;
    if (const char* configured = std::getenv("OPAL_TAILSCALE_CLI"); configured && *configured)
        add_candidate(candidates, configured);

    add_candidate(candidates, first_word(read_command_text("command -v tailscale 2>/dev/null")));
    add_candidate(candidates, "/usr/local/bin/tailscale");
    add_candidate(candidates, "/opt/homebrew/bin/tailscale");
    add_candidate(candidates, "/Applications/Tailscale.app/Contents/MacOS/Tailscale");

    if (const char* home = std::getenv("HOME"); home && *home)
        add_candidate(candidates, std::string(home) + "/Applications/Tailscale.app/Contents/MacOS/Tailscale");
    return candidates;
}

std::string tailscale_command_for(const std::string& executable, std::string_view arguments)
{
    if (executable.empty()) return {};
    return "TAILSCALE_BE_CLI=1 " + shell_quote(executable) + " " + std::string(arguments) + " 2>/dev/null";
}

std::string local_tailnet_ipv4_for(const std::string& executable)
{
    return first_tailnet_ipv4(read_command_text(tailscale_command_for(executable, "ip -4")));
}

std::string active_tailscale_cli_path()
{
    for (const auto& candidate : tailscale_cli_candidates())
        if (!local_tailnet_ipv4_for(candidate).empty()) return candidate;
    return {};
}

}

bool tailscale_cli_available()
{
    return !tailscale_cli_candidates().empty();
}

bool tailscale_connected()
{
    return !active_tailscale_cli_path().empty();
}

int require_tailscale()
{
    if (!tailscale_cli_available()) {
#if defined(OPAL_PLATFORM_MACOS)
        std::cerr << "OPAL requires Tailscale.\n"
                  << "Install: https://tailscale.com/download/mac\n"
                  << "Open Tailscale, complete the VPN setup, sign in, then run OPAL again.\n";
#else
        std::cerr << "OPAL requires Tailscale.\n"
                  << "Install:\n"
                  << "  curl -fsSL https://tailscale.com/install.sh | sh\n"
                  << "  sudo tailscale up\n"
                  << "Then run OPAL again.\n";
#endif
        return 1;
    }
    if (!tailscale_connected()) {
#if defined(OPAL_PLATFORM_MACOS)
        std::cerr << "Tailscale is installed but not connected. Open Tailscale and sign in/connect, then run OPAL again.\n";
#else
        std::cerr << "Tailscale is installed but not connected. Run 'sudo tailscale up', then run OPAL again.\n";
#endif
        return 1;
    }
    return 0;
}

std::vector<std::string> tailnet_peer_ipv4s()
{
    const auto executable = active_tailscale_cli_path();
    if (executable.empty()) return {};
    return parse_tailnet_status_ipv4s(
        read_command_text(tailscale_command_for(executable, "status --peers=true --self=false")));
}

std::string local_tailnet_ipv4()
{
    const auto executable = active_tailscale_cli_path();
    return executable.empty() ? std::string{} : local_tailnet_ipv4_for(executable);
}

}
