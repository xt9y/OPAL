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
    const auto source = read_all("src/platform/windows/system_backend.cpp");
    assert(source.find("CLSID_TaskScheduler") != std::string::npos);
    assert(source.find("TASK_TRIGGER_LOGON") != std::string::npos);
    assert(source.find("TASK_ACTION_EXEC") != std::string::npos);
    assert(source.find("TASK_LOGON_INTERACTIVE_TOKEN") != std::string::npos);
    assert(source.find("TASK_RUNLEVEL_HIGHEST") != std::string::npos);
    assert(source.find("MFTEnumEx") != std::string::npos);
    assert(source.find("MFT_ENUM_FLAG_HARDWARE") != std::string::npos);
    assert(source.find("CreateDXGIFactory1") != std::string::npos);
    assert(source.find("IMMDeviceEnumerator") != std::string::npos);
    assert(source.find("GetModuleFileNameW") != std::string::npos);
    assert(source.find("CreateService") == std::string::npos);
    return 0;
}
