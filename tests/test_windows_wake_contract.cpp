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
    const auto source = read_all("src/platform/windows/wake.cpp");
    assert(source.find("WSASocketW") != std::string::npos);
    assert(source.find("WSAPoll") != std::string::npos);
    assert(source.find("closesocket") != std::string::npos);
    assert(source.find("SO_BROADCAST") != std::string::npos);
    assert(source.find("hmac_sha256_hex") != std::string::npos);
    assert(source.find("accept(") != std::string::npos);
    assert(source.find("<unistd.h>") == std::string::npos);
    assert(source.find("fcntl(") == std::string::npos);
    return 0;
}
