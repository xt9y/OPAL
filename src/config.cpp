#include <opal/config.hpp>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace opal {
namespace {

#if defined(_WIN32)
std::filesystem::path windows_home()
{
    if (const char* local = std::getenv("LOCALAPPDATA"); local && *local)
        return std::filesystem::path(local) / "OPAL";
    if (const char* profile = std::getenv("USERPROFILE"); profile && *profile)
        return std::filesystem::path(profile) / ".opal";
    return std::filesystem::path(".opal");
}

bool regular_file(const std::filesystem::path& path)
{
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

std::vector<std::string> split_env(const char* value, char separator)
{
    std::vector<std::string> out;
    if (!value) return out;
    std::string_view text(value);
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto end = text.find(separator, begin);
        auto item = text.substr(begin, end == std::string_view::npos ? text.size() - begin : end - begin);
        if (!item.empty()) out.emplace_back(item);
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return out;
}

bool windows_executable_exists(const std::string& name)
{
    if (name.empty()) return false;
    const std::filesystem::path requested(name);
    if (requested.has_parent_path()) return regular_file(requested);

    auto extensions = split_env(std::getenv("PATHEXT"), ';');
    if (extensions.empty()) extensions = {".COM", ".EXE", ".BAT", ".CMD"};
    if (requested.has_extension()) extensions.insert(extensions.begin(), "");

    const auto paths = split_env(std::getenv("PATH"), ';');
    for (const auto& directory : paths) {
        for (const auto& extension : extensions) {
            if (regular_file(std::filesystem::path(directory) / (name + extension))) return true;
        }
    }
    return false;
}
#endif

}

std::string trim(std::string s)
{
    auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

Paths Paths::load()
{
    const char* override_home = std::getenv("OPAL_HOME");
    std::filesystem::path root;
    if (override_home && *override_home) root = override_home;
#if defined(_WIN32)
    else root = windows_home();
#else
    else {
        const char* home = std::getenv("HOME");
        root = (home && *home) ? std::filesystem::path(home) / ".opal" : std::filesystem::path(".opal");
    }
#endif
    return {root, root / "config.ini", root / "hosts.ini", root / "host.ini",
            root / "identity.key", root / "identity.pub", root / "authorized_clients",
            root / "tls.crt", root / "tls.key", root / "logs"};
}

bool ensure_layout(const Paths& paths)
{
    std::error_code error;
    std::filesystem::create_directories(paths.root, error);
    if (error) return false;
    std::filesystem::create_directories(paths.logs, error);
    if (error) return false;
#if !defined(_WIN32)
    (void)chmod(paths.root.c_str(), 0700);
#endif
    return true;
}

bool Ini::load(const std::filesystem::path& path)
{
    data_.clear();
    std::ifstream file(path);
    if (!file) return false;
    std::string section, line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        data_[section][trim(line.substr(0, equals))] = trim(line.substr(equals + 1));
    }
    return true;
}

bool Ini::save(const std::filesystem::path& path) const
{
    std::ofstream file(path, std::ios::trunc);
    if (!file) return false;
    for (const auto& [section, values] : data_) {
        if (!section.empty()) file << '[' << section << "]\n";
        for (const auto& [key, value] : values) file << key << '=' << value << "\n";
        file << "\n";
    }
    file.close();
#if !defined(_WIN32)
    (void)chmod(path.c_str(), 0600);
#endif
    return static_cast<bool>(file);
}

std::string Ini::get(const std::string& section, const std::string& key, const std::string& fallback) const
{
    auto section_it = data_.find(section);
    if (section_it == data_.end()) return fallback;
    auto key_it = section_it->second.find(key);
    return key_it == section_it->second.end() ? fallback : key_it->second;
}

int Ini::get_int(const std::string& section, const std::string& key, int fallback) const
{
    try { return std::stoi(get(section, key, std::to_string(fallback))); }
    catch (...) { return fallback; }
}

bool Ini::get_bool(const std::string& section, const std::string& key, bool fallback) const
{
    const auto value = get(section, key, fallback ? "true" : "false");
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

void Ini::set(const std::string& section, const std::string& key, const std::string& value)
{
    data_[section][key] = value;
}

std::string shell_quote(const std::string& value)
{
#if defined(_WIN32)
    std::string out = "\"";
    for (const char c : value) {
        if (c == '\"') out += "\\\"";
        else out += c;
    }
    out += '\"';
    return out;
#else
    std::string out = "'";
    for (const char c : value) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += '\'';
    return out;
#endif
}

bool command_exists(const std::string& name)
{
#if defined(_WIN32)
    if (name == "tailscale") {
        if (const char* configured = std::getenv("OPAL_TAILSCALE_CLI"); configured && *configured && regular_file(configured))
            return true;
        if (const char* program_files = std::getenv("ProgramFiles"); program_files && *program_files) {
            if (regular_file(std::filesystem::path(program_files) / "Tailscale" / "tailscale.exe")) return true;
        }
        if (const char* program_files_x86 = std::getenv("ProgramFiles(x86)"); program_files_x86 && *program_files_x86) {
            if (regular_file(std::filesystem::path(program_files_x86) / "Tailscale" / "tailscale.exe")) return true;
        }
    }
    return windows_executable_exists(name);
#else
    if (name == "tailscale") {
        if (const char* configured = std::getenv("OPAL_TAILSCALE_CLI"); configured && *configured && access(configured, X_OK) == 0)
            return true;
#ifdef __APPLE__
        for (const char* candidate : {"/usr/local/bin/tailscale", "/opt/homebrew/bin/tailscale", "/Applications/Tailscale.app/Contents/MacOS/Tailscale"})
            if (access(candidate, X_OK) == 0) return true;
        if (const char* home = std::getenv("HOME"); home && *home) {
            const std::string candidate = std::string(home) + "/Applications/Tailscale.app/Contents/MacOS/Tailscale";
            if (access(candidate.c_str(), X_OK) == 0) return true;
        }
#endif
    }
    const auto command = "command -v " + shell_quote(name) + " >/dev/null 2>&1";
    return std::system(command.c_str()) == 0;
#endif
}

}
