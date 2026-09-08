#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <opal/udp_transport.hpp>

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <poll.h>
#include <set>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

namespace opal {
namespace {

int native_fd(const UdpSocket &socket) noexcept
{
    if (!socket.valid() || socket.handle > static_cast<UdpNativeHandle>(INT_MAX)) return -1;
    return static_cast<int>(socket.handle);
}

bool endpoint_from_sockaddr(const sockaddr *address, socklen_t length, UdpEndpoint &endpoint)
{
    endpoint = {};
    if (!address || length == 0 || static_cast<std::size_t>(length) > endpoint.native.size()) return false;
    std::memcpy(endpoint.native.data(), address, length);
    endpoint.native_size = static_cast<std::uint32_t>(length);
    return true;
}

bool endpoint_to_sockaddr(const UdpEndpoint &endpoint, sockaddr_storage &address, socklen_t &length)
{
    address = {};
    length = 0;
    if (!endpoint.valid() || endpoint.native_size > sizeof(address)) return false;
    std::memcpy(&address, endpoint.native.data(), endpoint.native_size);
    length = static_cast<socklen_t>(endpoint.native_size);
    return true;
}

bool unsuitable_lan_interface(const char *name, unsigned flags)
{
    if (!name || !*name || (flags & IFF_LOOPBACK) || (flags & IFF_POINTOPOINT)) return true;
    const std::string_view value{name};
    constexpr std::string_view virtual_prefixes[] = {
        "docker", "veth", "virbr", "br-", "podman", "cni", "flannel", "tailscale", "wg", "tun", "tap", "zt"
    };
    for (const auto prefix : virtual_prefixes) if (value.starts_with(prefix)) return true;
    return false;
}

bool set_nonblocking_cloexec(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return false;
    const int fd_flags = fcntl(fd, F_GETFD, 0);
    return fd_flags >= 0 && fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) == 0;
}

}

UdpSocket open_udp_socket()
{
    const int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) return {};
    if (!set_nonblocking_cloexec(fd)) { close(fd); return {}; }

    int off = 0;
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) != 0) { close(fd); return {}; }

    int send_queue_bytes = kUdpQueueBufferBytes;
    int receive_queue_bytes = kUdpReceiveQueueBufferBytes;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_queue_bytes, sizeof(send_queue_bytes)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_queue_bytes, sizeof(receive_queue_bytes)) != 0) {
        close(fd);
        return {};
    }
#ifdef SO_RXQ_OVFL
    int overflow_reporting = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_RXQ_OVFL, &overflow_reporting, sizeof(overflow_reporting));
#endif

    int traffic_class = kUdpInteractiveTrafficClass;
    if (setsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS, &traffic_class, sizeof(traffic_class)) != 0) {
        close(fd);
        return {};
    }
    (void)setsockopt(fd, IPPROTO_IP, IP_TOS, &traffic_class, sizeof(traffic_class));

    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_any;
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) { close(fd); return {}; }
    socklen_t length = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) != 0) { close(fd); return {}; }

    return {static_cast<UdpNativeHandle>(fd), ntohs(address.sin6_port)};
}

void close_udp_socket(UdpSocket &socket)
{
    const int fd = native_fd(socket);
    if (fd >= 0) close(fd);
    socket.handle = kInvalidUdpHandle;
    socket.local_port = 0;
}

std::vector<UdpCandidate> local_udp_candidates(const UdpSocket &socket)
{
    std::vector<UdpCandidate> candidates;
    if (!socket.valid() || socket.local_port == 0) return candidates;

    ifaddrs *addresses = nullptr;
    if (getifaddrs(&addresses) != 0) return candidates;
    std::set<std::string> seen;
    for (auto *it = addresses; it; it = it->ifa_next) {
        if (!it->ifa_addr || !(it->ifa_flags & IFF_UP) || unsuitable_lan_interface(it->ifa_name, it->ifa_flags)) continue;
        const int family = it->ifa_addr->sa_family;
        char text[INET6_ADDRSTRLEN]{};
        if (family == AF_INET) {
            const auto *address = reinterpret_cast<const sockaddr_in *>(it->ifa_addr);
            if (!inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text))) continue;
        } else if (family == AF_INET6) {
            const auto *address = reinterpret_cast<const sockaddr_in6 *>(it->ifa_addr);
            if (IN6_IS_ADDR_UNSPECIFIED(&address->sin6_addr) || IN6_IS_ADDR_LOOPBACK(&address->sin6_addr) || IN6_IS_ADDR_LINKLOCAL(&address->sin6_addr)) continue;
            if (!inet_ntop(AF_INET6, &address->sin6_addr, text, sizeof(text))) continue;
        } else {
            continue;
        }
        if (seen.insert(text).second) candidates.push_back({text, socket.local_port});
    }
    freeifaddrs(addresses);
    return candidates;
}

bool resolve_udp_endpoint(const std::string &host, std::uint16_t port, UdpEndpoint &output)
{
    output = {};
    if (host.empty() || port == 0) return false;

    addrinfo hints{};
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo *result = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &result) != 0) return false;

    bool resolved = false;
    for (auto *it = result; it && !resolved; it = it->ai_next) {
        if (it->ai_family == AF_INET6) {
            resolved = endpoint_from_sockaddr(it->ai_addr, static_cast<socklen_t>(it->ai_addrlen), output);
        } else if (it->ai_family == AF_INET) {
            const auto *v4 = reinterpret_cast<const sockaddr_in *>(it->ai_addr);
            sockaddr_in6 mapped{};
            mapped.sin6_family = AF_INET6;
            mapped.sin6_port = v4->sin_port;
            mapped.sin6_addr.s6_addr[10] = 0xff;
            mapped.sin6_addr.s6_addr[11] = 0xff;
            std::memcpy(mapped.sin6_addr.s6_addr + 12, &v4->sin_addr, 4);
            resolved = endpoint_from_sockaddr(reinterpret_cast<const sockaddr *>(&mapped), sizeof(mapped), output);
        }
    }
    freeaddrinfo(result);
    return resolved;
}

bool udp_endpoint_numeric(const UdpEndpoint &endpoint, std::string &host, std::uint16_t &port)
{
    host.clear();
    port = 0;
    sockaddr_storage address{};
    socklen_t length = 0;
    if (!endpoint_to_sockaddr(endpoint, address, length)) return false;
    char host_text[NI_MAXHOST]{};
    char service_text[NI_MAXSERV]{};
    if (getnameinfo(reinterpret_cast<const sockaddr *>(&address), length,
                    host_text, sizeof(host_text), service_text, sizeof(service_text),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0) return false;
    try {
        const int parsed = std::stoi(service_text);
        if (parsed <= 0 || parsed > 65535) return false;
        host = host_text;
        port = static_cast<std::uint16_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool udp_endpoint_equal(const UdpEndpoint &a, const UdpEndpoint &b)
{
    sockaddr_storage aa{}, bb{};
    socklen_t al = 0, bl = 0;
    if (!endpoint_to_sockaddr(a, aa, al) || !endpoint_to_sockaddr(b, bb, bl)) return false;
    if (aa.ss_family != bb.ss_family) return false;
    if (aa.ss_family == AF_INET6) {
        const auto *x = reinterpret_cast<const sockaddr_in6 *>(&aa);
        const auto *y = reinterpret_cast<const sockaddr_in6 *>(&bb);
        return x->sin6_port == y->sin6_port && x->sin6_scope_id == y->sin6_scope_id &&
               std::memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(in6_addr)) == 0;
    }
    if (aa.ss_family == AF_INET) {
        const auto *x = reinterpret_cast<const sockaddr_in *>(&aa);
        const auto *y = reinterpret_cast<const sockaddr_in *>(&bb);
        return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
    }
    return al == bl && std::memcmp(&aa, &bb, al) == 0;
}

UdpSendResult classify_udp_send_result(std::ptrdiff_t written, std::size_t expected, int error_number)
{
    if (written == static_cast<std::ptrdiff_t>(expected)) return UdpSendResult::Sent;
    if (written < 0 && (error_number == EAGAIN || error_number == EWOULDBLOCK || error_number == ENOBUFS)) return UdpSendResult::WouldBlock;
    return UdpSendResult::Fatal;
}

UdpSendResult send_datagram_result(const UdpSocket &socket, const UdpEndpoint &endpoint, std::span<const std::uint8_t> data)
{
    const int fd = native_fd(socket);
    sockaddr_storage address{};
    socklen_t address_length = 0;
    if (fd < 0 || data.empty() || data.size() > 65507 || !endpoint_to_sockaddr(endpoint, address, address_length)) return UdpSendResult::Fatal;

    ssize_t written = 0;
    do {
        written = sendto(fd, data.data(), data.size(), MSG_DONTWAIT,
                         reinterpret_cast<const sockaddr *>(&address), address_length);
    } while (written < 0 && errno == EINTR);
    const int send_errno = written < 0 ? errno : 0;
    return classify_udp_send_result(static_cast<std::ptrdiff_t>(written), data.size(), send_errno);
}

UdpSendBatchResult send_datagrams_batch(const UdpSocket &socket, const UdpEndpoint &endpoint,
                                        std::span<const std::span<const std::uint8_t>> datagrams)
{
    const int fd = native_fd(socket);
    sockaddr_storage address{};
    socklen_t address_length = 0;
    if (fd < 0 || !endpoint_to_sockaddr(endpoint, address, address_length) || datagrams.empty() || datagrams.size() > kUdpSendBatchMax) return {};
    for (const auto datagram : datagrams) if (datagram.empty() || datagram.size() > 65507) return {};

#if defined(__linux__)
    std::array<mmsghdr, kUdpSendBatchMax> messages{};
    std::array<iovec, kUdpSendBatchMax> vectors{};
    for (std::size_t i = 0; i < datagrams.size(); ++i) {
        vectors[i].iov_base = const_cast<std::uint8_t *>(datagrams[i].data());
        vectors[i].iov_len = datagrams[i].size();
        messages[i].msg_hdr.msg_name = &address;
        messages[i].msg_hdr.msg_namelen = address_length;
        messages[i].msg_hdr.msg_iov = &vectors[i];
        messages[i].msg_hdr.msg_iovlen = 1;
    }
    int sent = 0;
    do {
        sent = sendmmsg(fd, messages.data(), static_cast<unsigned int>(datagrams.size()), MSG_DONTWAIT);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0) {
        const auto result = (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) ? UdpSendResult::WouldBlock : UdpSendResult::Fatal;
        return {0, result};
    }
    const auto count = static_cast<std::size_t>(sent);
    return {count, count == datagrams.size() ? UdpSendResult::Sent : UdpSendResult::WouldBlock};
#else
    std::size_t sent = 0;
    for (const auto datagram : datagrams) {
        const auto result = send_datagram_result(socket, endpoint, datagram);
        if (result != UdpSendResult::Sent) return {sent, result};
        ++sent;
    }
    return {sent, UdpSendResult::Sent};
#endif
}

bool send_datagram(const UdpSocket &socket, const UdpEndpoint &endpoint, std::span<const std::uint8_t> data)
{
    return send_datagram_result(socket, endpoint, data) == UdpSendResult::Sent;
}

int recv_datagram(const UdpSocket &socket, std::span<std::uint8_t> data, UdpEndpoint &source, int timeout_ms)
{
    const int fd = native_fd(socket);
    source = {};
    if (fd < 0 || data.empty()) return -1;

    pollfd descriptor{fd, POLLIN, 0};
    int rc = 0;
    do { rc = poll(&descriptor, 1, std::max(0, timeout_ms)); } while (rc < 0 && errno == EINTR);
    if (rc == 0) return -2;
    if (rc < 0 || !(descriptor.revents & POLLIN)) return -1;

    sockaddr_storage native_source{};
    socklen_t source_length = sizeof(native_source);
    ssize_t received = 0;
    do {
        received = recvfrom(fd, data.data(), data.size(), 0,
                            reinterpret_cast<sockaddr *>(&native_source), &source_length);
    } while (received < 0 && errno == EINTR);
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -2;
    if (received < 0) return -1;
    if (!endpoint_from_sockaddr(reinterpret_cast<const sockaddr *>(&native_source), source_length, source)) return -1;
    return static_cast<int>(received);
}

int recv_datagrams_batch(const UdpSocket &socket, std::span<UdpReceiveSlot> slots, int timeout_ms)
{
    const int fd = native_fd(socket);
    if (fd < 0 || slots.empty()) return -1;
    const std::size_t count = std::min(slots.size(), kUdpReceiveBatchMax);
    for (std::size_t i = 0; i < count; ++i) {
        if (slots[i].buffer.empty()) return -1;
        slots[i].size = 0;
        slots[i].source = {};
        slots[i].kernel_drops = 0;
    }

    pollfd descriptor{fd, POLLIN, 0};
    int rc = 0;
    do { rc = poll(&descriptor, 1, std::max(0, timeout_ms)); } while (rc < 0 && errno == EINTR);
    if (rc == 0) return 0;
    if (rc < 0 || !(descriptor.revents & POLLIN)) return -1;

#if defined(__linux__)
    std::array<mmsghdr, kUdpReceiveBatchMax> messages{};
    std::array<iovec, kUdpReceiveBatchMax> vectors{};
    std::array<sockaddr_storage, kUdpReceiveBatchMax> sources{};
#ifdef SO_RXQ_OVFL
    constexpr std::size_t control_bytes = CMSG_SPACE(sizeof(std::uint32_t));
    std::array<std::array<unsigned char, control_bytes>, kUdpReceiveBatchMax> controls{};
#endif
    for (std::size_t i = 0; i < count; ++i) {
        vectors[i].iov_base = slots[i].buffer.data();
        vectors[i].iov_len = slots[i].buffer.size();
        messages[i].msg_hdr.msg_name = &sources[i];
        messages[i].msg_hdr.msg_namelen = sizeof(sources[i]);
        messages[i].msg_hdr.msg_iov = &vectors[i];
        messages[i].msg_hdr.msg_iovlen = 1;
#ifdef SO_RXQ_OVFL
        messages[i].msg_hdr.msg_control = controls[i].data();
        messages[i].msg_hdr.msg_controllen = controls[i].size();
#endif
    }

    int received = 0;
    do {
        received = recvmmsg(fd, messages.data(), static_cast<unsigned int>(count), MSG_DONTWAIT, nullptr);
    } while (received < 0 && errno == EINTR);
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if (received < 0) return -1;

    for (int i = 0; i < received; ++i) {
        auto &slot = slots[static_cast<std::size_t>(i)];
        slot.size = messages[static_cast<std::size_t>(i)].msg_len;
        if (!endpoint_from_sockaddr(reinterpret_cast<const sockaddr *>(&sources[static_cast<std::size_t>(i)]),
                                    messages[static_cast<std::size_t>(i)].msg_hdr.msg_namelen, slot.source)) return -1;
#ifdef SO_RXQ_OVFL
        for (cmsghdr *cmsg = CMSG_FIRSTHDR(&messages[static_cast<std::size_t>(i)].msg_hdr); cmsg;
             cmsg = CMSG_NXTHDR(&messages[static_cast<std::size_t>(i)].msg_hdr, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_RXQ_OVFL &&
                cmsg->cmsg_len >= CMSG_LEN(sizeof(std::uint32_t))) {
                std::uint32_t drops = 0;
                std::memcpy(&drops, CMSG_DATA(cmsg), sizeof(drops));
                slot.kernel_drops = drops;
                break;
            }
        }
#endif
    }
    return received;
#else
    int received = 0;
    for (std::size_t i = 0; i < count; ++i) {
        auto &slot = slots[i];
        const int n = recv_datagram(socket, slot.buffer, slot.source, i == 0 ? timeout_ms : 0);
        if (n == -2) break;
        if (n < 0) return received ? received : -1;
        slot.size = static_cast<std::size_t>(n);
        ++received;
    }
    return received;
#endif
}

}
