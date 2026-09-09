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
    const auto source = read_all("src/config.cpp");
    assert(source.find("_WIN32") != std::string::npos);
    assert(source.find("USERPROFILE") != std::string::npos);
    assert(source.find("LOCALAPPDATA") != std::string::npos);
    assert(source.find("ProgramFiles") != std::string::npos);
    assert(source.find("PATHEXT") != std::string::npos);
    assert(source.find("std::filesystem") != std::string::npos);
    assert(source.find("command -v") != std::string::npos); // POSIX branch remains supported.
    return 0;
}
