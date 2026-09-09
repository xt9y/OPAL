#pragma once

#include <opal/udp_transport.hpp>

#include <cstdint>
#include <string>

namespace opal {

UdpSocket open_udp_listener(std::uint16_t port, const std::string& bind_host, std::string& error);
UdpSocket duplicate_udp_socket(const UdpSocket& socket);
bool set_udp_broadcast(const UdpSocket& socket, bool enabled);

}
