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

    const auto wake = read_all("src/wake.cpp");
    assert(wake.find("SOCK_DGRAM|SOCK_CLOEXEC") == std::string::npos);
    assert(wake.find("set_socket_no_sigpipe") != std::string::npos);
    assert(wake.find("send_flags_no_sigpipe") != std::string::npos);
    return 0;
}
