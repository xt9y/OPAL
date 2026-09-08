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
    const auto capture = read_all("src/video_capture.cpp");
    const auto pipewire = read_all("src/pipewire_capture.cpp");

    assert(capture.find("native_pipewire_authorization_lost()") != std::string::npos);
    assert(capture.find("Linux screen authorization lost") != std::string::npos);
    assert(pipewire.find("void NativePipeWireVideoCapture::stop()") != std::string::npos);

    const auto stop = pipewire.find("void NativePipeWireVideoCapture::stop()");
    const auto stop_body = pipewire.substr(stop);
    assert(stop_body.find("xdp_portal_create_screencast_session") == std::string::npos);
    assert(stop_body.find("xdp_session_start") == std::string::npos);

    return 0;
}
