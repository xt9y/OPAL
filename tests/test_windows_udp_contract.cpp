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
    const auto source = read_all("src/platform/windows/udp_transport.cpp");
    assert(source.find("<winsock2.h>") != std::string::npos);
    assert(source.find("<ws2tcpip.h>") != std::string::npos);
    assert(source.find("<iphlpapi.h>") != std::string::npos);
    assert(source.find("WSAStartup") != std::string::npos);
    assert(source.find("WSASocket") != std::string::npos);
    assert(source.find("ioctlsocket") != std::string::npos);
    assert(source.find("WSASendTo") != std::string::npos);
    assert(source.find("WSARecvFrom") != std::string::npos);
    assert(source.find("WSAPoll") != std::string::npos);
    assert(source.find("GetAdaptersAddresses") != std::string::npos);
    assert(source.find("closesocket") != std::string::npos);
    assert(source.find("sendmmsg") == std::string::npos);
    assert(source.find("recvmmsg") == std::string::npos);
    assert(source.find("getifaddrs") == std::string::npos);
    assert(source.find("poll(") == std::string::npos);
    return 0;
}
