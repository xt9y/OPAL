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
    const auto source = read_all("src/platform/windows/host.cpp");
    assert(source.find("GetModuleFileNameW") != std::string::npos);
    assert(source.find("opal-input.exe") != std::string::npos);
    assert(source.find("_putenv_s") != std::string::npos);
    assert(source.find("#include \"../../host.cpp\"") != std::string::npos);
    assert(source.find("windows_host_setup_impl") != std::string::npos);
    assert(source.find("windows_host_run_impl") != std::string::npos);
    assert(source.find("windows_host_daemon_impl") != std::string::npos);
    return 0;
}
