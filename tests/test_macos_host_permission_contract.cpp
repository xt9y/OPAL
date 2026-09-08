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
    assert(host.find("CGRequestScreenCaptureAccess") != std::string::npos);
    assert(host.find("AXIsProcessTrusted") != std::string::npos);
    assert(host.find("AXIsProcessTrustedWithOptions") != std::string::npos);
    assert(host.find("kAXTrustedCheckOptionPrompt") != std::string::npos);
    assert(host.find("Screen Recording permission") != std::string::npos);
    assert(host.find("Accessibility permission") != std::string::npos);
    assert(host.find("macos_host_setup_impl") != std::string::npos);
    assert(host.find("macos_host_run_impl") != std::string::npos);

    const auto doctor = read_all("src/platform/macos/system_backend.mm");
    assert(doctor.find("VTCopyVideoEncoderList") != std::string::npos);
    assert(doctor.find("kVTVideoEncoderList_IsHardwareAccelerated") != std::string::npos);
    assert(doctor.find("VideoToolbox H.264 hardware encoder") != std::string::npos);
    assert(doctor.find("VTIsHardwareDecodeSupported") != std::string::npos);
    assert(doctor.find("de.xt9y.opal.host") != std::string::npos);
    assert(doctor.find("LaunchAgents") != std::string::npos);
    assert(doctor.find("--internal-host-daemon") != std::string::npos);
    assert(doctor.find("bootstrap ") != std::string::npos);
    assert(doctor.find("kickstart -k") != std::string::npos);
    assert(doctor.find("systemctl") == std::string::npos);

    const auto setup = read_all("src/setup.cpp");
    assert(setup.find("opal doctor") != std::string::npos);
    assert(setup.find("systemctl --user status") == std::string::npos);
    assert(setup.find("current_platform()==PlatformKind::Linux") != std::string::npos);
    return 0;
}
