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
    const auto host = read_all("src/host.cpp");
    assert(host.find("#include <opal/pipewire_capture.hpp>") != std::string::npos);
    assert(host.find("map_composite_pointer") != std::string::npos);
    assert(host.find("remap_pointer_line") != std::string::npos);
    assert(host.find("line.rfind(\"MOUSE \",0)==0") != std::string::npos);
    return 0;
}
