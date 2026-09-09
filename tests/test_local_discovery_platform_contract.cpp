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
    const auto source = read_all("src/local_discovery.cpp");
    assert(source.find("<sys/socket.h>") == std::string::npos);
    assert(source.find("<arpa/inet.h>") == std::string::npos);
    assert(source.find("<fcntl.h>") == std::string::npos);
    assert(source.find("<unistd.h>") == std::string::npos);
    assert(source.find("open_udp_listener") != std::string::npos);
    assert(source.find("duplicate_udp_socket") != std::string::npos);
    assert(source.find("set_udp_broadcast") != std::string::npos);

    const auto header = read_all("include/opal/udp_socket_ops.hpp");
    assert(header.find("open_udp_listener") != std::string::npos);
    assert(header.find("duplicate_udp_socket") != std::string::npos);
    assert(header.find("set_udp_broadcast") != std::string::npos);
    assert(header.find("sockaddr") == std::string::npos);
    assert(header.find("SOCKET") == std::string::npos);
    return 0;
}
