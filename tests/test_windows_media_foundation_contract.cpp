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
    const auto source = read_all("src/platform/windows/video_encoder_backend.cpp");
    assert(source.find("MFTEnumEx") != std::string::npos);
    assert(source.find("MFT_ENUM_FLAG_HARDWARE") != std::string::npos);
    assert(source.find("MFCreateDXGIDeviceManager") != std::string::npos);
    assert(source.find("MFT_MESSAGE_SET_D3D_MANAGER") != std::string::npos);
    assert(source.find("D3D11_VIDEO_PROCESSOR_CONTENT_DESC") != std::string::npos);
    assert(source.find("DXGI_FORMAT_NV12") != std::string::npos);
    assert(source.find("MFCreateDXGISurfaceBuffer") != std::string::npos);
    assert(source.find("CODECAPI_AVLowLatencyMode") != std::string::npos);
    assert(source.find("CODECAPI_AVEncVideoForceKeyFrame") != std::string::npos);
    assert(source.find("CODECAPI_AVEncCommonMeanBitRate") != std::string::npos);
    assert(source.find("ProcessInput") != std::string::npos);
    assert(source.find("ProcessOutput") != std::string::npos);
    assert(source.find("avcodec") == std::string::npos);
    return 0;
}
