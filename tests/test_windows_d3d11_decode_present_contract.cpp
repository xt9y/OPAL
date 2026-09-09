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
    const auto decoder = read_all("src/platform/windows/video_decoder.cpp");
    assert(decoder.find("AV_HWDEVICE_TYPE_D3D11VA") != std::string::npos);
    assert(decoder.find("AV_PIX_FMT_D3D11") != std::string::npos);
    assert(decoder.find("hardware-d3d11va+direct") != std::string::npos);
    assert(decoder.find("AV_PIX_FMT_DRM_PRIME") == std::string::npos);

    const auto presenter = read_all("src/platform/windows/video_present.cpp");
    assert(presenter.find("SDL_PROP_WINDOW_WIN32_HWND_POINTER") != std::string::npos);
    assert(presenter.find("AV_PIX_FMT_D3D11") != std::string::npos);
    assert(presenter.find("frame->data[0]") != std::string::npos);
    assert(presenter.find("frame->data[1]") != std::string::npos);
    assert(presenter.find("CreateSwapChainForHwnd") != std::string::npos);
    assert(presenter.find("CreateVideoProcessorInputView") != std::string::npos);
    assert(presenter.find("VideoProcessorBlt") != std::string::npos);
    assert(presenter.find("DXGI_PRESENT_ALLOW_TEARING") != std::string::npos);
    assert(presenter.find("av_hwframe_transfer_data") == std::string::npos);
    return 0;
}
