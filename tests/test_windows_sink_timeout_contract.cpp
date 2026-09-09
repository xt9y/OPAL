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
    const auto media = read_all("src/platform/windows/media.cpp");
    const auto header = read_all("include/opal/media.hpp");

    assert(media.find("CreateNamedPipeW") != std::string::npos);
    assert(media.find("FILE_FLAG_OVERLAPPED") != std::string::npos);
    assert(media.find("OVERLAPPED overlapped") != std::string::npos);
    assert(media.find("CancelIoEx") != std::string::npos);
    assert(media.find("GetOverlappedResult") != std::string::npos);
    assert(media.find("sink.wait_io") != std::string::npos);
    assert(header.find("ProcessIoHandle wait_io") != std::string::npos);
    return 0;
}
