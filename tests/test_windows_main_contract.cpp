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
    const auto source = read_all("src/main.cpp");
    assert(source.find("Windows") != std::string::npos);
    assert(source.find("#if !defined(_WIN32)") != std::string::npos);
    assert(source.find("SIGPIPE") != std::string::npos);
    assert(source.find("signal(SIGPIPE") != std::string::npos);
    return 0;
}
