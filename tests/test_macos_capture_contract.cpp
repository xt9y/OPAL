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
    assert(text.find("SCStreamDelegate") != std::string::npos);
    assert(text.find("didStopWithError") != std::string::npos);
    assert(text.find("CMSampleBuffer") != std::string::npos);
    assert(text.find("CVPixelBuffer") != std::string::npos);
    assert(text.find("CGMainDisplayID") != std::string::npos);
    assert(text.find("candidate.displayID == main_id") != std::string::npos);
    assert(text.find("queueDepth = 3") != std::string::npos);
    assert(text.find("latest_ = std::move(frame)") != std::string::npos);
    assert(text.find("kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange") != std::string::npos);
    assert(text.find("SCStreamFrameInfoDisplayTime") != std::string::npos);
    assert(text.find("mach_absolute_time") != std::string::npos);
    assert(text.find("mach_timebase_info") != std::string::npos);
    assert(text.find("CaptureTimestampQuality::Exact") != std::string::npos);
    assert(text.find("anchor_pts_") == std::string::npos);
    assert(text.find("dispatch_release(content_sem)") != std::string::npos);
    assert(text.find("dispatch_release(start_sem)") != std::string::npos);
    assert(text.find("dispatch_release(stop_sem)") != std::string::npos);
    assert(text.find("dispatch_release(queue_)") != std::string::npos);
    assert(text.find("PermissionDenied") != std::string::npos);

    const auto facade = read_all("src/platform/macos/video_capture.cpp");
    assert(facade.find("terminal = true") != std::string::npos);
    assert(facade.find("last_platform_error") != std::string::npos);
    return 0;
}
