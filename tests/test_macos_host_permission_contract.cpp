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
    const auto text = read_all("src/platform/macos/host.cpp");
    assert(text.find("CGPreflightScreenCaptureAccess") != std::string::npos);
    assert(text.find("AXIsProcessTrusted") != std::string::npos);
    assert(text.find("Screen Recording permission") != std::string::npos);
    assert(text.find("Accessibility permission") != std::string::npos);
    assert(text.find("macos_host_run_impl") != std::string::npos);
    return 0;
}
