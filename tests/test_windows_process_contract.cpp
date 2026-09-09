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
    const auto source = read_all("src/platform/windows/media.cpp");
    assert(source.find("CreateProcessW") != std::string::npos);
    assert(source.find("CreatePipe") != std::string::npos);
    assert(source.find("SetHandleInformation") != std::string::npos);
    assert(source.find("WriteFile") != std::string::npos);
    assert(source.find("ReadFile") != std::string::npos);
    assert(source.find("PeekNamedPipe") != std::string::npos);
    assert(source.find("WaitForSingleObject") != std::string::npos);
    assert(source.find("TerminateProcess") != std::string::npos);
    assert(source.find("fork(") == std::string::npos);
    assert(source.find("posix_spawn") == std::string::npos);
    return 0;
}
