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
    assert(source.find("SM_CXVIRTUALSCREEN") != std::string::npos);
    assert(source.find("SM_CYVIRTUALSCREEN") != std::string::npos);
    assert(source.find("windows_virtual_desktop_mode") != std::string::npos);
    assert(source.find("#define SDL_GetDesktopDisplayMode windows_virtual_desktop_mode") != std::string::npos);
    return 0;
}
