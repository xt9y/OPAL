#include <cassert>
#include <fstream>
#include <sstream>
#include <string>

int main() {
    std::ifstream f("systemd/opal-host.service");
    assert(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    const auto unit = ss.str();
    assert(unit.find("ExecStart=/usr/local/bin/opal host daemon") != std::string::npos);
    assert(unit.find("Restart=always") != std::string::npos);
    assert(unit.find("Restart=on-failure") == std::string::npos);
    return 0;
}
