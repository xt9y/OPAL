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
    const auto source = read_all("src/platform/windows/capture_backend.cpp");

    assert(source.find("EnumAdapters1") != std::string::npos);
    assert(source.find("EnumOutputs") != std::string::npos);
    assert(source.find("AttachedToDesktop") != std::string::npos);
    assert(source.find("std::vector<OutputCapture>") != std::string::npos);
    assert(source.find("DesktopCoordinates") != std::string::npos);
    assert(source.find("virtual_desktop_") != std::string::npos);
    assert(source.find("composite_texture_") != std::string::npos);
    assert(source.find("CreateVideoProcessorInputView") != std::string::npos);
    assert(source.find("CreateVideoProcessorOutputView") != std::string::npos);
    assert(source.find("VideoProcessorBlt") != std::string::npos);
    assert(source.find("CopyResource") != std::string::npos);
    assert(source.find("Map(") == std::string::npos);
    assert(source.find("D3D11_USAGE_STAGING") == std::string::npos);
    assert(source.find("D3D11_CPU_ACCESS_READ") == std::string::npos);

    return 0;
}
