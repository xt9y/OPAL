#include <opal/crypto.hpp>
#include <cassert>
#include <fstream>
#include <iterator>
#include <string>

static std::string read_all(const char*path){
    std::ifstream in(path);
    assert(in.good());
    return std::string((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
}

int main(){
    const auto code=opal::pairing_code();
    assert(code.size()==19);
    assert(code[4]=='-'&&code[9]=='-'&&code[14]=='-');

    const auto input=read_all("src/input_helper.cpp");
    assert(input.find("O_CLOEXEC")!=std::string::npos);
    assert(input.find("O_NONBLOCK")==std::string::npos);

    const auto wake=read_all("src/wake.cpp");
    assert(wake.find("SO_RCVTIMEO")!=std::string::npos);
    assert(wake.find("SO_SNDTIMEO")!=std::string::npos);
    assert(wake.find("secret.empty()")!=std::string::npos);

    const auto client=read_all("src/client.cpp");
    assert(client.find("i<300")!=std::string::npos);

    const auto readme=read_all("README.md");
    assert(readme.find("mktemp -d")!=std::string::npos);
    assert(readme.find("/tmp/zrok2.tar.gz")==std::string::npos);
    assert(readme.find("tar -xzf /tmp/zrok2.tar.gz -C /tmp")==std::string::npos);

    const auto ci=read_all(".github/workflows/ci.yml");
    assert(ci.find("sanitize")!=std::string::npos);
    assert(ci.find("stress")!=std::string::npos);
    return 0;
}
