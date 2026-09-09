#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include <opal/udp_socket_ops.hpp>

#include <cstdint>
#include <cstring>
#include <mutex>

namespace opal {
namespace {

bool winsock_ready()
{
    static std::once_flag once;
    static bool ready = false;
    std::call_once(once, [] {
        WSADATA data{};
        ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    });
    return ready;
}

SOCKET native_socket(const UdpSocket& socket)
{
    return socket.valid() ? static_cast<SOCKET>(static_cast<std::uintptr_t>(socket.handle)) : INVALID_SOCKET;
}

UdpNativeHandle portable_socket(SOCKET socket)
{
    return socket == INVALID_SOCKET ? kInvalidUdpHandle
                                    : static_cast<UdpNativeHandle>(static_cast<std::uintptr_t>(socket));
}

bool configure_socket(SOCKET socket)
{
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) == SOCKET_ERROR) return false;
    DWORD false_value = FALSE;
    DWORD returned = 0;
    (void)WSAIoctl(socket, SIO_UDP_CONNRESET, &false_value, sizeof(false_value), nullptr, 0,
                   &returned, nullptr, nullptr);
    int off = 0;
    if (setsockopt(socket, IPPROTO_IPV6, IPV6_V6ONLY,
                   reinterpret_cast<const char*>(&off), sizeof(off)) == SOCKET_ERROR) return false;
    int one = 1;
    (void)setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    int queue = kUdpQueueBufferBytes;
    if (setsockopt(socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&queue), sizeof(queue)) == SOCKET_ERROR ||
        setsockopt(socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&queue), sizeof(queue)) == SOCKET_ERROR) return false;
    int traffic_class = kUdpInteractiveTrafficClass;
#ifdef IPV6_TCLASS
    (void)setsockopt(socket, IPPROTO_IPV6, IPV6_TCLASS,
                     reinterpret_cast<const char*>(&traffic_class), sizeof(traffic_class));
#endif
    (void)setsockopt(socket, IPPROTO_IP, IP_TOS,
                     reinterpret_cast<const char*>(&traffic_class), sizeof(traffic_class));
    return true;
}

}

UdpSocket open_udp_listener(std::uint16_t port, const std::string& bind_host, std::string& error)
{
    error.clear();
    if (!winsock_ready()) { error = "Winsock initialization failed"; return {}; }
    const SOCKET socket = WSASocketW(AF_INET6, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (socket == INVALID_SOCKET) { error = "local discovery socket failed"; return {}; }
    if (!configure_socket(socket)) {
        closesocket(socket); error = "local discovery socket configuration failed"; return {};
    }

    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_port = htons(port);
    address.sin6_addr = in6addr_any;
    if (!bind_host.empty() && bind_host != "0.0.0.0" && bind_host != "::") {
        IN_ADDR ipv4{};
        if (InetPtonA(AF_INET, bind_host.c_str(), &ipv4) != 1) {
            closesocket(socket); error = "invalid local discovery bind address"; return {};
        }
        address.sin6_addr.u.Byte[10] = 0xff;
        address.sin6_addr.u.Byte[11] = 0xff;
        std::memcpy(address.sin6_addr.u.Byte + 12, &ipv4, sizeof(ipv4));
    }

    if (bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        closesocket(socket); error = "local discovery bind failed"; return {};
    }
    int length = sizeof(address);
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) == SOCKET_ERROR) {
        closesocket(socket); error = "local discovery socket query failed"; return {};
    }
    return {portable_socket(socket), ntohs(address.sin6_port)};
}

UdpSocket duplicate_udp_socket(const UdpSocket& socket)
{
    const SOCKET native = native_socket(socket);
    if (native == INVALID_SOCKET || !winsock_ready()) return {};
    WSAPROTOCOL_INFOW protocol{};
    if (WSADuplicateSocketW(native, GetCurrentProcessId(), &protocol) == SOCKET_ERROR) return {};
    const SOCKET duplicate = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
                                        &protocol, 0, WSA_FLAG_OVERLAPPED);
    if (duplicate == INVALID_SOCKET) return {};
    return {portable_socket(duplicate), socket.local_port};
}

bool set_udp_broadcast(const UdpSocket& socket, bool enabled)
{
    const SOCKET native = native_socket(socket);
    if (native == INVALID_SOCKET) return false;
    int value = enabled ? 1 : 0;
    return setsockopt(native, SOL_SOCKET, SO_BROADCAST,
                      reinterpret_cast<const char*>(&value), sizeof(value)) != SOCKET_ERROR;
}

}
