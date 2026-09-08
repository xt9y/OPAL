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
    const auto text = read_all("src/platform/macos/capture_backend.mm");
    assert(text.find("SCShareableContent") != std::string::npos);
    assert(text.find("SCStreamConfiguration") != std::string::npos);
    assert(text.find("SCStreamOutputTypeScreen") != std::string::npos);
    assert(text.find("CMSampleBuffer") != std::string::npos);
    assert(text.find("CVPixelBuffer") != std::string::npos);
    assert(text.find("queueDepth = 3") != std::string::npos);
    assert(text.find("latest_ = std::move(frame)") != std::string::npos);
    assert(text.find("kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange") != std::string::npos);
    assert(text.find("PermissionDenied") != std::string::npos);
    return 0;
}
