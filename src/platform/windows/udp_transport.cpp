#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mstcpip.h>
#include <mswsock.h>

#include <opal/udp_transport.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

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

SOCKET native_socket(const UdpSocket& socket) noexcept
{
    if (!socket.valid()) return INVALID_SOCKET;
    return static_cast<SOCKET>(static_cast<std::uintptr_t>(socket.handle));
}

UdpNativeHandle portable_socket(SOCKET socket) noexcept
{
    if (socket == INVALID_SOCKET) return kInvalidUdpHandle;
    return static_cast<UdpNativeHandle>(static_cast<std::uintptr_t>(socket));
}

bool endpoint_from_sockaddr(const sockaddr* address, int length, UdpEndpoint& endpoint)
{
    endpoint = {};
    if (!address || length <= 0 || static_cast<std::size_t>(length) > endpoint.native.size()) return false;
    std::memcpy(endpoint.native.data(), address, static_cast<std::size_t>(length));
    endpoint.native_size = static_cast<std::uint32_t>(length);
    return true;
}

bool endpoint_to_sockaddr(const UdpEndpoint& endpoint, sockaddr_storage& address, int& length)
{
    address = {};
    length = 0;
    if (!endpoint.valid() || endpoint.native_size > sizeof(address)) return false;
    std::memcpy(&address, endpoint.native.data(), endpoint.native_size);
    length = static_cast<int>(endpoint.native_size);
    return true;
}

bool virtual_adapter_name(const char* name)
{
    if (!name || !*name) return false;
    const std::string_view value{name};
    constexpr std::string_view prefixes[] = {
        "TUN", "Wintun", "WireGuard", "Tailscale", "ZeroTier", "Hyper-V", "vEthernet", "Npcap"
    };
    for (const auto prefix : prefixes) if (value.starts_with(prefix)) return true;
    return false;
}

bool unsuitable_adapter(const IP_ADAPTER_ADDRESSES& adapter)
{
    if (adapter.OperStatus != IfOperStatusUp) return true;
    if (adapter.IfType == IF_TYPE_SOFTWARE_LOOPBACK || adapter.IfType == IF_TYPE_TUNNEL) return true;
    return virtual_adapter_name(adapter.AdapterName);
}

bool would_block_error(int error_number)
{
    if (error_number == WSAEWOULDBLOCK || error_number == WSAENOBUFS) return true;
#ifdef EAGAIN
    if (error_number == EAGAIN) return true;
#endif
#ifdef EWOULDBLOCK
    if (error_number == EWOULDBLOCK) return true;
#endif
#ifdef ENOBUFS
    if (error_number == ENOBUFS) return true;
#endif
    return false;
}

int wait_readable(SOCKET socket, int timeout_ms)
{
    WSAPOLLFD descriptor{};
    descriptor.fd = socket;
    descriptor.events = POLLRDNORM;
    const int rc = WSAPoll(&descriptor, 1, std::max(0, timeout_ms));
    if (rc == 0) return 0;
    if (rc == SOCKET_ERROR) return -1;
    if ((descriptor.revents & (POLLRDNORM | POLLIN)) == 0) return -1;
    return 1;
}

}

UdpSocket open_udp_socket()
{
    if (!winsock_ready()) return {};

    const SOCKET socket = WSASocketW(AF_INET6, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (socket == INVALID_SOCKET) return {};

    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        closesocket(socket);
        return {};
    }

    DWORD false_value = FALSE;
    DWORD returned = 0;
    (void)WSAIoctl(socket, SIO_UDP_CONNRESET, &false_value, sizeof(false_value), nullptr, 0,
                   &returned, nullptr, nullptr);

    int off = 0;
    if (setsockopt(socket, IPPROTO_IPV6, IPV6_V6ONLY,
                   reinterpret_cast<const char*>(&off), sizeof(off)) == SOCKET_ERROR) {
        closesocket(socket);
        return {};
    }

    int send_queue_bytes = kUdpQueueBufferBytes;
    int receive_queue_bytes = kUdpReceiveQueueBufferBytes;
    if (setsockopt(socket, SOL_SOCKET, SO_SNDBUF,
                   reinterpret_cast<const char*>(&send_queue_bytes), sizeof(send_queue_bytes)) == SOCKET_ERROR ||
        setsockopt(socket, SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&receive_queue_bytes), sizeof(receive_queue_bytes)) == SOCKET_ERROR) {
        closesocket(socket);
        return {};
    }

    int traffic_class = kUdpInteractiveTrafficClass;
#ifdef IPV6_TCLASS
    (void)setsockopt(socket, IPPROTO_IPV6, IPV6_TCLASS,
                     reinterpret_cast<const char*>(&traffic_class), sizeof(traffic_class));
#endif
    (void)setsockopt(socket, IPPROTO_IP, IP_TOS,
                     reinterpret_cast<const char*>(&traffic_class), sizeof(traffic_class));

    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_any;
    if (bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        closesocket(socket);
        return {};
    }

    int length = sizeof(address);
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) == SOCKET_ERROR) {
        closesocket(socket);
        return {};
    }

    return {portable_socket(socket), ntohs(address.sin6_port)};
}

void close_udp_socket(UdpSocket& socket)
{
    const SOCKET native = native_socket(socket);
    if (native != INVALID_SOCKET) closesocket(native);
    socket.handle = kInvalidUdpHandle;
    socket.local_port = 0;
}

std::vector<UdpCandidate> local_udp_candidates(const UdpSocket& socket)
{
    std::vector<UdpCandidate> candidates;
    if (!socket.valid() || socket.local_port == 0 || !winsock_ready()) return candidates;

    ULONG bytes = 16 * 1024;
    std::vector<std::byte> storage(bytes);
    IP_ADAPTER_ADDRESSES* adapters = nullptr;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        storage.resize(bytes);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        result = GetAdaptersAddresses(AF_UNSPEC,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, adapters, &bytes);
    }
    if (result != NO_ERROR || !adapters) return candidates;

    std::set<std::string> seen;
    for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
        if (unsuitable_adapter(*adapter)) continue;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            const auto* address = unicast->Address.lpSockaddr;
            if (!address) continue;
            char text[INET6_ADDRSTRLEN]{};
            if (address->sa_family == AF_INET) {
                const auto* v4 = reinterpret_cast<const sockaddr_in*>(address);
                const auto first = reinterpret_cast<const unsigned char*>(&v4->sin_addr.s_addr)[0];
                if (v4->sin_addr.s_addr == INADDR_ANY || first == 127) continue;
                if (!InetNtopA(AF_INET, const_cast<IN_ADDR*>(&v4->sin_addr), text, sizeof(text))) continue;
            } else if (address->sa_family == AF_INET6) {
                const auto* v6 = reinterpret_cast<const sockaddr_in6*>(address);
                if (IN6_IS_ADDR_UNSPECIFIED(&v6->sin6_addr) || IN6_IS_ADDR_LOOPBACK(&v6->sin6_addr) ||
                    IN6_IS_ADDR_LINKLOCAL(&v6->sin6_addr)) continue;
                if (!InetNtopA(AF_INET6, const_cast<IN6_ADDR*>(&v6->sin6_addr), text, sizeof(text))) continue;
            } else {
                continue;
            }
            if (seen.insert(text).second) candidates.push_back({text, socket.local_port});
        }
    }
    return candidates;
}

bool resolve_udp_endpoint(const std::string& host, std::uint16_t port, UdpEndpoint& output)
{
    output = {};
    if (host.empty() || port == 0 || !winsock_ready()) return false;

    addrinfo hints{};
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &result) != 0) return false;

    bool resolved = false;
    for (auto* it = result; it && !resolved; it = it->ai_next) {
        if (it->ai_family == AF_INET6) {
            resolved = endpoint_from_sockaddr(it->ai_addr, static_cast<int>(it->ai_addrlen), output);
        } else if (it->ai_family == AF_INET) {
            const auto* v4 = reinterpret_cast<const sockaddr_in*>(it->ai_addr);
            sockaddr_in6 mapped{};
            mapped.sin6_family = AF_INET6;
            mapped.sin6_port = v4->sin_port;
            mapped.sin6_addr.s6_addr[10] = 0xff;
            mapped.sin6_addr.s6_addr[11] = 0xff;
            std::memcpy(mapped.sin6_addr.s6_addr + 12, &v4->sin_addr, 4);
            resolved = endpoint_from_sockaddr(reinterpret_cast<const sockaddr*>(&mapped), sizeof(mapped), output);
        }
    }
    freeaddrinfo(result);
    return resolved;
}

bool udp_endpoint_numeric(const UdpEndpoint& endpoint, std::string& host, std::uint16_t& port)
{
    host.clear();
    port = 0;
    sockaddr_storage address{};
    int length = 0;
    if (!endpoint_to_sockaddr(endpoint, address, length)) return false;

    char host_text[NI_MAXHOST]{};
    char service_text[NI_MAXSERV]{};
    if (getnameinfo(reinterpret_cast<const sockaddr*>(&address), length,
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

bool udp_endpoint_equal(const UdpEndpoint& a, const UdpEndpoint& b)
{
    sockaddr_storage aa{}, bb{};
    int al = 0, bl = 0;
    if (!endpoint_to_sockaddr(a, aa, al) || !endpoint_to_sockaddr(b, bb, bl)) return false;
    if (aa.ss_family != bb.ss_family) return false;
    if (aa.ss_family == AF_INET6) {
        const auto* x = reinterpret_cast<const sockaddr_in6*>(&aa);
        const auto* y = reinterpret_cast<const sockaddr_in6*>(&bb);
        return x->sin6_port == y->sin6_port && x->sin6_scope_id == y->sin6_scope_id &&
               std::memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(IN6_ADDR)) == 0;
    }
    if (aa.ss_family == AF_INET) {
        const auto* x = reinterpret_cast<const sockaddr_in*>(&aa);
        const auto* y = reinterpret_cast<const sockaddr_in*>(&bb);
        return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
    }
    return al == bl && std::memcmp(&aa, &bb, static_cast<std::size_t>(al)) == 0;
}

UdpSendResult classify_udp_send_result(std::ptrdiff_t written, std::size_t expected, int error_number)
{
    if (written == static_cast<std::ptrdiff_t>(expected)) return UdpSendResult::Sent;
    if (written < 0 && would_block_error(error_number)) return UdpSendResult::WouldBlock;
    return UdpSendResult::Fatal;
}

UdpSendResult send_datagram_result(const UdpSocket& socket, const UdpEndpoint& endpoint,
                                   std::span<const std::uint8_t> data)
{
    const SOCKET native = native_socket(socket);
    sockaddr_storage address{};
    int address_length = 0;
    if (native == INVALID_SOCKET || data.empty() || data.size() > 65507 ||
        !endpoint_to_sockaddr(endpoint, address, address_length)) return UdpSendResult::Fatal;

    WSABUF buffer{};
    buffer.buf = reinterpret_cast<char*>(const_cast<std::uint8_t*>(data.data()));
    buffer.len = static_cast<ULONG>(data.size());
    DWORD written = 0;
    const int rc = WSASendTo(native, &buffer, 1, &written, 0,
                             reinterpret_cast<const sockaddr*>(&address), address_length,
                             nullptr, nullptr);
    if (rc == 0) return classify_udp_send_result(static_cast<std::ptrdiff_t>(written), data.size(), 0);
    return classify_udp_send_result(-1, data.size(), WSAGetLastError());
}

UdpSendBatchResult send_datagrams_batch(const UdpSocket& socket, const UdpEndpoint& endpoint,
                                        std::span<const std::span<const std::uint8_t>> datagrams)
{
    if (datagrams.empty() || datagrams.size() > kUdpSendBatchMax) return {};
    std::size_t sent = 0;
    for (const auto datagram : datagrams) {
        if (datagram.empty() || datagram.size() > 65507) return {sent, UdpSendResult::Fatal};
        const auto result = send_datagram_result(socket, endpoint, datagram);
        if (result != UdpSendResult::Sent) return {sent, result};
        ++sent;
    }
    return {sent, UdpSendResult::Sent};
}

bool send_datagram(const UdpSocket& socket, const UdpEndpoint& endpoint,
                   std::span<const std::uint8_t> data)
{
    return send_datagram_result(socket, endpoint, data) == UdpSendResult::Sent;
}

int recv_datagram(const UdpSocket& socket, std::span<std::uint8_t> data,
                  UdpEndpoint& source, int timeout_ms)
{
    const SOCKET native = native_socket(socket);
    source = {};
    if (native == INVALID_SOCKET || data.empty()) return -1;
    const int wait = wait_readable(native, timeout_ms);
    if (wait == 0) return -2;
    if (wait < 0) return -1;

    sockaddr_storage native_source{};
    int source_length = sizeof(native_source);
    WSABUF buffer{};
    buffer.buf = reinterpret_cast<char*>(data.data());
    buffer.len = static_cast<ULONG>(data.size());
    DWORD received = 0;
    DWORD flags = 0;
    const int rc = WSARecvFrom(native, &buffer, 1, &received, &flags,
                               reinterpret_cast<sockaddr*>(&native_source), &source_length,
                               nullptr, nullptr);
    if (rc == SOCKET_ERROR) return would_block_error(WSAGetLastError()) ? -2 : -1;
    if (!endpoint_from_sockaddr(reinterpret_cast<const sockaddr*>(&native_source), source_length, source)) return -1;
    return static_cast<int>(received);
}

int recv_datagrams_batch(const UdpSocket& socket, std::span<UdpReceiveSlot> slots, int timeout_ms)
{
    const SOCKET native = native_socket(socket);
    if (native == INVALID_SOCKET || slots.empty()) return -1;
    const std::size_t count = std::min(slots.size(), kUdpReceiveBatchMax);
    for (std::size_t i = 0; i < count; ++i) {
        if (slots[i].buffer.empty()) return -1;
        slots[i].size = 0;
        slots[i].source = {};
        slots[i].source_length = 0;
        slots[i].kernel_drops = 0;
    }

    const int wait = wait_readable(native, timeout_ms);
    if (wait == 0) return 0;
    if (wait < 0) return -1;

    std::size_t received_count = 0;
    for (; received_count < count; ++received_count) {
        auto& slot = slots[received_count];
        sockaddr_storage native_source{};
        int source_length = sizeof(native_source);
        WSABUF buffer{};
        buffer.buf = reinterpret_cast<char*>(slot.buffer.data());
        buffer.len = static_cast<ULONG>(slot.buffer.size());
        DWORD received = 0;
        DWORD flags = 0;
        const int rc = WSARecvFrom(native, &buffer, 1, &received, &flags,
                                   reinterpret_cast<sockaddr*>(&native_source), &source_length,
                                   nullptr, nullptr);
        if (rc == SOCKET_ERROR) {
            const int error_number = WSAGetLastError();
            if (would_block_error(error_number)) break;
            return received_count > 0 ? static_cast<int>(received_count) : -1;
        }
        if (!endpoint_from_sockaddr(reinterpret_cast<const sockaddr*>(&native_source), source_length, slot.source))
            return received_count > 0 ? static_cast<int>(received_count) : -1;
        slot.source_length = slot.source.native_size;
        slot.size = static_cast<std::size_t>(received);
    }
    return static_cast<int>(received_count);
}

}
