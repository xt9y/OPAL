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
    const auto windows = read_all("Makefile.windows");
    const auto macos = read_all("Makefile.macos");
    const auto shared = read_all("src/native_video_capture.cpp");

    assert(windows.find("src/native_video_capture.cpp") != std::string::npos);
    assert(macos.find("src/native_video_capture.cpp") != std::string::npos);
    assert(windows.find("src/platform/macos/video_capture.cpp") == std::string::npos);
    assert(macos.find("src/platform/macos/video_capture.cpp") == std::string::npos);
    assert(shared.find("native video capture failed to start") != std::string::npos);
    assert(shared.find("native system audio capture failed to start") != std::string::npos);
    assert(shared.find("macOS") == std::string::npos);
    return 0;
}
