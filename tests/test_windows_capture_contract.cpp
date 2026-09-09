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
    assert(source.find("D3D11CreateDevice") != std::string::npos);
    assert(source.find("DuplicateOutput") != std::string::npos);
    assert(source.find("AcquireNextFrame") != std::string::npos);
    assert(source.find("ReleaseFrame") != std::string::npos);
    assert(source.find("DXGI_ERROR_WAIT_TIMEOUT") != std::string::npos);
    assert(source.find("DXGI_ERROR_ACCESS_LOST") != std::string::npos);
    assert(source.find("QueryPerformanceCounter") != std::string::npos);
    assert(source.find("NativeVideoFrameKind::Opaque") != std::string::npos);
    assert(source.find("GetDC(") == std::string::npos);
    assert(source.find("BitBlt") == std::string::npos);
    assert(source.find("GetDIBits") == std::string::npos);
    return 0;
}
