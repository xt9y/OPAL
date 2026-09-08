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
    const auto setup = read_all("src/setup.cpp");
    assert(setup.find("Remember selected screens for automatic hosting? [Y/n]") != std::string::npos);
    assert(setup.find("remember_screens") != std::string::npos);
    assert(setup.find("native_pipewire_prepare") != std::string::npos);
    assert(setup.find("portal-session.token") != std::string::npos);
    return 0;
}
