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
    const auto crypto = read_all("src/crypto.cpp");
    const auto host = read_all("src/host.cpp");

    assert(crypto.find("#if !defined(_WIN32)") != std::string::npos);
    assert(crypto.find("chmod(priv.c_str()") != std::string::npos);
    assert(crypto.find("chmod(pub.c_str()") != std::string::npos);

    const auto host_chmod = host.find("chmod(G.authorized.c_str()") ;
    assert(host_chmod != std::string::npos);
    const auto host_guard = host.rfind("#if !defined(_WIN32)", host_chmod);
    const auto host_endif = host.find("#endif", host_guard);
    assert(host_guard != std::string::npos && host_endif != std::string::npos && host_endif > host_chmod);

    assert(host.find("#if defined(_WIN32)\n    return e && *e ? e : \"opal-input.exe\";") != std::string::npos);
    return 0;
}
