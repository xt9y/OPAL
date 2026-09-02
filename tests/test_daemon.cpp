#include <opal/host.hpp>
#include <cassert>
#include <fstream>
#include <sstream>
#include <string>

static_assert(opal::video_backpressure_timeout_ms<=750);

int main() {
    std::ifstream f("systemd/opal-host.service");
    assert(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    const auto unit = ss.str();
    assert(unit.find("ExecStart=/usr/local/bin/opal --internal-host-daemon") != std::string::npos);
    assert(unit.find("Restart=always") != std::string::npos);
    assert(unit.find("Restart=on-failure") == std::string::npos);
    return 0;
}
