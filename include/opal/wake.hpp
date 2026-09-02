#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace opal {
std::vector<uint8_t> wol_packet(const std::string &mac);
bool send_wol(const std::string &mac,const std::string &broadcast="255.255.255.255",uint16_t port=9);
int run_bridge(uint16_t port);
int wake_named(const std::string &name);
}
