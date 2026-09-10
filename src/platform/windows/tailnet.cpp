#include <opal/config.hpp>
#include <opal/tailnet.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace opal {
namespace {

std::string quote_command_arg(std::string_view value)
{
    std::string out = "\"";
    for (const char c : value) {
        if (c == '\"') out += "\\\"";
        else out += c;
    }
    out += '\"';
    return out;
}

std::string read_command_text(const std::string& command)
{
    if (command.empty()) return {};
    FILE* pipe = _popen(command.c_str(), "r");
    if (!pipe) return {};
    std::array<char, 512> buffer{};
    std::string text;
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) text += buffer.data();
    (void)_pclose(pipe);
    return text;
}

bool path_exists(const std::string& path)
{
    std::error_code error;
    return !path.empty() && std::filesystem::is_regular_file(path, error) && !error;
}

void add_candidate(std::vector<std::string>& candidates, std::string path, bool allow_path_lookup = false)
{
    if (path.empty()) return;
    if (!path_exists(path) && !(allow_path_lookup && command_exists(path))) return;
    if (std::find(candidates.begin(), candidates.end(), path) == candidates.end())
        candidates.push_back(std::move(path));
}

std::vector<std::string> tailscale_cli_candidates()
{
    std::vector<std::string> candidates;
    if (const char* configured = std::getenv("OPAL_TAILSCALE_CLI"); configured && *configured)
        add_candidate(candidates, configured);

    if (const char* program_files = std::getenv("ProgramFiles"); program_files && *program_files)
        add_candidate(candidates, (std::filesystem::path(program_files) / "Tailscale" / "tailscale.exe").string());
    if (const char* program_files_x86 = std::getenv("ProgramFiles(x86)"); program_files_x86 && *program_files_x86)
        add_candidate(candidates, (std::filesystem::path(program_files_x86) / "Tailscale" / "tailscale.exe").string());

    add_candidate(candidates, "tailscale.exe", true);
    return candidates;
}

std::string tailscale_command_for(const std::string& executable, std::string_view arguments)
{
    if (executable.empty()) return {};
    return quote_command_arg(executable) + " " + std::string(arguments) + " 2>NUL";
}

std::string first_tailnet_ipv4(const std::string& text)
{
    std::istringstream values(text);
    std::string value;
    while (values >> value) if (is_tailnet_ipv4(value)) return value;
    return {};
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
        std::cerr << "OPAL requires Tailscale.\n"
                  << "Install: https://tailscale.com/download/windows\n"
                  << "Run the installer, open Tailscale from the system tray, sign in, then run OPAL again.\n";
        return 1;
    }
    if (!tailscale_connected()) {
        std::cerr << "Tailscale is installed but not connected. Open Tailscale from the system tray and sign in/connect, then run OPAL again.\n";
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
