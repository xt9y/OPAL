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
    const auto windows_host = read_all("src/platform/windows/host.cpp");

    const auto crypto_chmod = crypto.find("chmod(priv.c_str()");
    assert(crypto_chmod != std::string::npos);
    const auto crypto_guard = crypto.rfind("#if !defined(_WIN32)", crypto_chmod);
    const auto crypto_endif = crypto.find("#endif", crypto_chmod);
    assert(crypto_guard != std::string::npos && crypto_endif != std::string::npos && crypto_endif > crypto_chmod);

    assert(host.find("chmod(G.authorized.c_str()") != std::string::npos);
    assert(windows_host.find("windows_ignore_posix_mode") != std::string::npos);
    assert(windows_host.find("#define chmod(path, mode) windows_ignore_posix_mode(path, mode)") != std::string::npos);
    assert(windows_host.find("#undef chmod") != std::string::npos);
    assert(windows_host.find("#include <sys/stat.h>") < windows_host.find("#define chmod(path, mode)"));
    return 0;
}
