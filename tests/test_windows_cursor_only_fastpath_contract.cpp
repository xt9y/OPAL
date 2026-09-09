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
    const auto source = read_all("src/platform/windows/capture_backend.cpp");
    const auto guard = source.find("if (!desktop_updated && output.ready)");
    const auto copy = source.find("CopyResource(output.latest_texture, texture)", guard);
    assert(guard != std::string::npos);
    assert(copy != std::string::npos);
    assert(source.find("return AcquireResult::PointerUpdated;", guard) < copy);
    return 0;
}
