#include <opal/udp_socket_ops.hpp>

#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace opal {
namespace {

int native_fd(const UdpSocket& socket)
{
    return socket.valid() ? static_cast<int>(socket.handle) : -1;
}

bool set_nonblocking_cloexec(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return false;
    const int fd_flags = fcntl(fd, F_GETFD, 0);
    return fd_flags >= 0 && fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) == 0;
}

}

UdpSocket open_udp_listener(std::uint16_t port, const std::string& bind_host, std::string& error)
{
    error.clear();
    const int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) { error = "local discovery socket failed"; return {}; }
    if (!set_nonblocking_cloexec(fd)) { close(fd); error = "local discovery socket flags unavailable"; return {}; }

    int off = 0;
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) != 0) {
        close(fd); error = "local discovery dual-stack unavailable"; return {};
    }
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    int queue = kUdpQueueBufferBytes;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &queue, sizeof(queue)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &queue, sizeof(queue)) != 0) {
        close(fd); error = "local discovery socket buffers unavailable"; return {};
    }
    int traffic_class = kUdpInteractiveTrafficClass;
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS, &traffic_class, sizeof(traffic_class)) != 0) {
        close(fd); error = "local discovery traffic class unavailable"; return {};
    }
    (void)setsockopt(fd, IPPROTO_IP, IP_TOS, &traffic_class, sizeof(traffic_class));

    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_port = htons(port);
    address.sin6_addr = in6addr_any;
    if (!bind_host.empty() && bind_host != "0.0.0.0" && bind_host != "::") {
        in_addr ipv4{};
        if (inet_pton(AF_INET, bind_host.c_str(), &ipv4) != 1) {
            close(fd); error = "invalid local discovery bind address"; return {};
        }
        address.sin6_addr.s6_addr[10] = 0xff;
        address.sin6_addr.s6_addr[11] = 0xff;
        std::memcpy(address.sin6_addr.s6_addr + 12, &ipv4, sizeof(ipv4));
    }

    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(fd); error = "local discovery bind failed"; return {};
    }
    socklen_t length = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        close(fd); error = "local discovery socket query failed"; return {};
    }
    return {static_cast<UdpNativeHandle>(fd), ntohs(address.sin6_port)};
}

UdpSocket duplicate_udp_socket(const UdpSocket& socket)
{
    const int fd = native_fd(socket);
    if (fd < 0) return {};
    const int duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    return duplicate < 0 ? UdpSocket{} : UdpSocket{static_cast<UdpNativeHandle>(duplicate), socket.local_port};
}

bool set_udp_broadcast(const UdpSocket& socket, bool enabled)
{
    const int fd = native_fd(socket);
    if (fd < 0) return false;
    int value = enabled ? 1 : 0;
    return setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &value, sizeof(value)) == 0;
}

}
