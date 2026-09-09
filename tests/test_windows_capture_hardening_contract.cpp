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

    assert(source.find("fail_start(") != std::string::npos);
    assert(source.find("desktop spans multiple DXGI adapters") != std::string::npos);
    assert(source.find("D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ROTATION") != std::string::npos);
    assert(source.find("GetVideoProcessorCaps") != std::string::npos);
    assert(source.find("VideoProcessorSetStreamRotation") != std::string::npos);
    assert(source.find("DXGI_MODE_ROTATION_ROTATE90") != std::string::npos);
    assert(source.find("D3D11_VIDEO_PROCESSOR_ROTATION_90") != std::string::npos);
    assert(source.find("DXGI_MODE_ROTATION_ROTATE180") != std::string::npos);
    assert(source.find("D3D11_VIDEO_PROCESSOR_ROTATION_180") != std::string::npos);
    assert(source.find("DXGI_MODE_ROTATION_ROTATE270") != std::string::npos);
    assert(source.find("D3D11_VIDEO_PROCESSOR_ROTATION_270") != std::string::npos);

    return 0;
}
