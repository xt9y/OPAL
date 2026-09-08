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
    const auto host = read_all("src/platform/macos/host.cpp");
    assert(host.find("CGPreflightScreenCaptureAccess") != std::string::npos);
    assert(host.find("AXIsProcessTrusted") != std::string::npos);
    assert(host.find("Screen Recording permission") != std::string::npos);
    assert(host.find("Accessibility permission") != std::string::npos);
    assert(host.find("macos_host_run_impl") != std::string::npos);

    const auto doctor = read_all("src/platform/macos/system_backend.mm");
    assert(doctor.find("VTCopyVideoEncoderList") != std::string::npos);
    assert(doctor.find("kVTVideoEncoderList_IsHardwareAccelerated") != std::string::npos);
    assert(doctor.find("VideoToolbox H.264 hardware encoder") != std::string::npos);
    assert(doctor.find("VTIsHardwareDecodeSupported") != std::string::npos);
    return 0;
}
