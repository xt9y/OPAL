#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static std::string read_all(const std::filesystem::path& path)
{
    std::ifstream in(path);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

int main()
{
    const std::vector<std::string> forbidden = {
        "<linux/",
        "<arpa/inet.h>",
        "<ifaddrs.h>",
        "<sys/socket.h>",
        "sockaddr_storage",
        "socklen_t",
        "CGEventRef",
        "CVPixelBufferRef",
        "CMSampleBufferRef",
        "SCStream",
        "VTCompressionSessionRef",
        "NSPasteboard",
        "HANDLE"
    };

    bool clean = true;
    for (const auto& entry : std::filesystem::recursive_directory_iterator("include/opal")) {
        if (!entry.is_regular_file() || entry.path().extension() != ".hpp") continue;
        const auto text = read_all(entry.path());
        for (const auto& token : forbidden) {
            if (text.find(token) == std::string::npos) continue;
            std::cerr << entry.path().string() << ": forbidden public platform token: " << token << '\n';
            clean = false;
        }
    }
    assert(clean);

    const auto tailnet_header = read_all("include/opal/tailnet.hpp");
    const auto tailnet_source = read_all("src/tailnet.cpp");
    const auto config_source = read_all("src/config.cpp");
    assert(tailnet_header.find("popen") == std::string::npos);
    assert(tailnet_header.find("pclose") == std::string::npos);
    assert(tailnet_header.find("FILE*") == std::string::npos);
    assert(tailnet_header.find("<cstdio>") == std::string::npos);
    assert(tailnet_source.find("popen") != std::string::npos);
    assert(tailnet_source.find("tailscale ip -4") != std::string::npos);
    assert(tailnet_source.find("/Applications/Tailscale.app/Contents/MacOS/Tailscale") != std::string::npos);
    assert(tailnet_source.find("TAILSCALE_BE_CLI=1") != std::string::npos);
    assert(tailnet_source.find("OPAL_TAILSCALE_CLI") != std::string::npos);
    assert(config_source.find("/Applications/Tailscale.app/Contents/MacOS/Tailscale") != std::string::npos);

    const auto wake = read_all("src/wake.cpp");
    assert(wake.find("SOCK_DGRAM|SOCK_CLOEXEC") == std::string::npos);
    assert(wake.find("set_socket_no_sigpipe") != std::string::npos);
    assert(wake.find("send_flags_no_sigpipe") != std::string::npos);
    return 0;
}
