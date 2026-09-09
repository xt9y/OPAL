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
    const auto capture = read_all("src/platform/windows/capture_backend.cpp");
    const auto cursor = read_all("src/platform/windows/cursor_compositor.hpp");
    const auto source = capture + cursor;

    assert(capture.find("CursorUpdate::PointerUpdated") != std::string::npos);
    assert(capture.find("cursor_.update") != std::string::npos);
    assert(capture.find("cursor_.draw") != std::string::npos);
    assert(source.find("LastMouseUpdateTime") != std::string::npos);
    assert(source.find("PointerPosition.Visible") != std::string::npos);
    assert(source.find("PointerShapeBufferSize") != std::string::npos);
    assert(source.find("GetFramePointerShape") != std::string::npos);
    assert(source.find("DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR") != std::string::npos);
    assert(source.find("DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR") != std::string::npos);
    assert(source.find("DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME") != std::string::npos);
    assert(source.find("D3DCompile") != std::string::npos);
    assert(source.find("CreatePixelShader") != std::string::npos);
    assert(source.find("cursor_target_texture_") != std::string::npos);
    assert(source.find("cursor_ops_texture_") != std::string::npos);
    assert(source.find("UpdateSubresource(cursor_constants_") != std::string::npos);
    assert(source.find("D3D11_USAGE_STAGING") == std::string::npos);
    assert(source.find("D3D11_CPU_ACCESS_READ") == std::string::npos);

    return 0;
}
