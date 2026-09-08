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
    const auto text = read_all("src/platform/macos/video_encoder_backend.mm");
    assert(text.find("VTCompressionSessionRef") != std::string::npos);
    assert(text.find("kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder") != std::string::npos);
    assert(text.find("kVTVideoEncoderSpecification_EnableLowLatencyRateControl") != std::string::npos);
    assert(text.find("kVTCompressionPropertyKey_RealTime") != std::string::npos);
    assert(text.find("kVTCompressionPropertyKey_AllowFrameReordering") != std::string::npos);
    assert(text.find("kVTCompressionPropertyKey_AverageBitRate") != std::string::npos);
    assert(text.find("kVTCompressionPropertyKey_ExpectedFrameRate") != std::string::npos);
    assert(text.find("kVTCompressionPropertyKey_MaxKeyFrameInterval") == std::string::npos);
    assert(text.find("kVTEncodeFrameOptionKey_ForceKeyFrame") != std::string::npos);
    assert(text.find("CMVideoFormatDescriptionGetH264ParameterSetAtIndex") != std::string::npos);
    assert(text.find("std::shared_ptr<void> pixel_owner") != std::string::npos);
    assert(text.find("std::condition_variable ready") != std::string::npos);
    assert(text.find("ready.wait_for") != std::string::npos);
    assert(text.find("dispatch_semaphore_create(0)") == std::string::npos);
    assert(text.find("force_idr_.exchange") != std::string::npos);
    assert(text.find("VTCompressionSessionInvalidate") != std::string::npos);
    assert(text.find("ffmpeg") == std::string::npos);
    return 0;
}
