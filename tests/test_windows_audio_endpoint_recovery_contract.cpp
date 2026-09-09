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
    const auto source = read_all("src/platform/windows/audio_capture_backend.cpp");

    assert(source.find("IMMNotificationClient") != std::string::npos);
    assert(source.find("RegisterEndpointNotificationCallback") != std::string::npos);
    assert(source.find("UnregisterEndpointNotificationCallback") != std::string::npos);
    assert(source.find("OnDefaultDeviceChanged") != std::string::npos);
    assert(source.find("endpoint_change_pending_") != std::string::npos);
    assert(source.find("recover_default_endpoint") != std::string::npos);
    assert(source.find("AUDCLNT_E_DEVICE_INVALIDATED") != std::string::npos);
    assert(source.find("++config_revision_") != std::string::npos);
    assert(source.find("config_revision_ = 1") == std::string::npos);

    return 0;
}
