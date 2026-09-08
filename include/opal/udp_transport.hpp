#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace opal {

constexpr int kUdpQueueBufferBytes = 64 * 1024;
constexpr int kUdpReceiveQueueBufferBytes = 256 * 1024;
constexpr int kUdpInteractiveTrafficClass = 0xb8;
constexpr std::size_t kUdpReceiveBatchMax = 32;
constexpr std::size_t kUdpSendBatchMax = 32;

enum class UdpSendResult : std::uint8_t { Sent = 0, WouldBlock, Fatal };

struct UdpSendBatchResult {
    std::size_t sent = 0;
    UdpSendResult result = UdpSendResult::Fatal;
};

struct UdpCandidate {
    std::string host;
    std::uint16_t port = 0;
};

using UdpNativeHandle = std::intptr_t;
inline constexpr UdpNativeHandle kInvalidUdpHandle = -1;

struct UdpSocket {
    union {
        UdpNativeHandle handle;
        UdpNativeHandle fd; // transitional source alias; not a POSIX type
    };
    std::uint16_t local_port = 0;

    constexpr UdpSocket() noexcept : handle(kInvalidUdpHandle) {}
    constexpr UdpSocket(UdpNativeHandle native, std::uint16_t port) noexcept : handle(native), local_port(port) {}
    bool valid() const noexcept { return handle != kInvalidUdpHandle; }
};

struct UdpEndpoint {
    std::array<std::byte, 128> native{};
    std::uint32_t native_size = 0;

    bool valid() const noexcept { return native_size > 0 && native_size <= native.size(); }
};

struct UdpReceiveSlot {
    std::span<std::uint8_t> buffer{};
    UdpEndpoint source{};
    std::uint32_t source_length = 0; // compatibility mirror of source.native_size
    std::size_t size = 0;
    std::uint32_t kernel_drops = 0;
};

UdpSocket open_udp_socket();
void close_udp_socket(UdpSocket &);
std::vector<UdpCandidate> local_udp_candidates(const UdpSocket &);
bool resolve_udp_endpoint(const std::string &, std::uint16_t, UdpEndpoint &);
bool udp_endpoint_numeric(const UdpEndpoint &, std::string &host, std::uint16_t &port);
bool udp_endpoint_equal(const UdpEndpoint &, const UdpEndpoint &);
UdpSendResult classify_udp_send_result(std::ptrdiff_t written, std::size_t expected, int error_number);
UdpSendResult send_datagram_result(const UdpSocket &, const UdpEndpoint &, std::span<const std::uint8_t>);
UdpSendBatchResult send_datagrams_batch(const UdpSocket &, const UdpEndpoint &,
                                        std::span<const std::span<const std::uint8_t>>);
bool send_datagram(const UdpSocket &, const UdpEndpoint &, std::span<const std::uint8_t>);
int recv_datagram(const UdpSocket &, std::span<std::uint8_t>, UdpEndpoint &, int timeout_ms);
int recv_datagrams_batch(const UdpSocket &, std::span<UdpReceiveSlot>, int timeout_ms);

// Transitional source-compatible wrappers. They use OPAL's opaque handle and
// endpoint types only, so no POSIX socket type leaks into public headers.
inline bool resolve_udp_endpoint(const std::string &host, std::uint16_t port,
                                 UdpEndpoint &endpoint, std::uint32_t &native_size)
{
    const bool ok = resolve_udp_endpoint(host, port, endpoint);
    native_size = ok ? endpoint.native_size : 0;
    return ok;
}

inline UdpSendResult send_datagram_result(UdpNativeHandle handle, const UdpEndpoint &endpoint,
                                          std::uint32_t, std::span<const std::uint8_t> data)
{
    return send_datagram_result(UdpSocket{handle, 0}, endpoint, data);
}

inline UdpSendBatchResult send_datagrams_batch(UdpNativeHandle handle, const UdpEndpoint &endpoint,
                                               std::uint32_t,
                                               std::span<const std::span<const std::uint8_t>> datagrams)
{
    return send_datagrams_batch(UdpSocket{handle, 0}, endpoint, datagrams);
}

inline bool send_datagram(UdpNativeHandle handle, const UdpEndpoint &endpoint, std::uint32_t,
                          std::span<const std::uint8_t> data)
{
    return send_datagram(UdpSocket{handle, 0}, endpoint, data);
}

inline int recv_datagram(UdpNativeHandle handle, std::span<std::uint8_t> data,
                         UdpEndpoint &source, std::uint32_t &source_length, int timeout_ms)
{
    const int result = recv_datagram(UdpSocket{handle, 0}, data, source, timeout_ms);
    source_length = source.native_size;
    return result;
}

inline int recv_datagrams_batch(UdpNativeHandle handle, std::span<UdpReceiveSlot> slots, int timeout_ms)
{
    const int result = recv_datagrams_batch(UdpSocket{handle, 0}, slots, timeout_ms);
    if (result > 0) {
        for (int i = 0; i < result; ++i) slots[static_cast<std::size_t>(i)].source_length = slots[static_cast<std::size_t>(i)].source.native_size;
    }
    return result;
}

template <class NativeAddress, class NativeLength>
requires (!std::is_same_v<std::remove_cvref_t<NativeAddress>, UdpEndpoint> &&
          std::is_trivially_copyable_v<std::remove_cvref_t<NativeAddress>> &&
          std::is_integral_v<std::remove_cvref_t<NativeLength>>)
inline bool resolve_udp_endpoint(const std::string &host, std::uint16_t port,
                                 NativeAddress &address, NativeLength &length)
{
    UdpEndpoint endpoint{};
    if (!resolve_udp_endpoint(host, port, endpoint) || endpoint.native_size > sizeof(address)) {
        length = 0;
        return false;
    }
    std::memset(&address, 0, sizeof(address));
    std::memcpy(&address, endpoint.native.data(), endpoint.native_size);
    length = static_cast<NativeLength>(endpoint.native_size);
    return true;
}

template <class NativeAddress, class NativeLength>
requires (!std::is_same_v<std::remove_cvref_t<NativeAddress>, UdpEndpoint> &&
          std::is_trivially_copyable_v<std::remove_cvref_t<NativeAddress>> &&
          std::is_integral_v<std::remove_cvref_t<NativeLength>>)
inline UdpEndpoint udp_endpoint_from_native(const NativeAddress &address, NativeLength length)
{
    UdpEndpoint endpoint{};
    const auto bytes = static_cast<std::size_t>(length);
    if (bytes == 0 || bytes > sizeof(address) || bytes > endpoint.native.size()) return endpoint;
    std::memcpy(endpoint.native.data(), &address, bytes);
    endpoint.native_size = static_cast<std::uint32_t>(bytes);
    return endpoint;
}

template <class NativeAddress, class NativeLength>
requires (!std::is_same_v<std::remove_cvref_t<NativeAddress>, UdpEndpoint>)
inline UdpSendResult send_datagram_result(UdpNativeHandle handle, const NativeAddress &address,
                                          NativeLength length, std::span<const std::uint8_t> data)
{
    return send_datagram_result(UdpSocket{handle, 0}, udp_endpoint_from_native(address, length), data);
}

template <class NativeAddress, class NativeLength>
requires (!std::is_same_v<std::remove_cvref_t<NativeAddress>, UdpEndpoint>)
inline UdpSendBatchResult send_datagrams_batch(UdpNativeHandle handle, const NativeAddress &address,
                                               NativeLength length,
                                               std::span<const std::span<const std::uint8_t>> datagrams)
{
    return send_datagrams_batch(UdpSocket{handle, 0}, udp_endpoint_from_native(address, length), datagrams);
}

template <class NativeAddress, class NativeLength>
requires (!std::is_same_v<std::remove_cvref_t<NativeAddress>, UdpEndpoint>)
inline bool send_datagram(UdpNativeHandle handle, const NativeAddress &address, NativeLength length,
                          std::span<const std::uint8_t> data)
{
    return send_datagram(UdpSocket{handle, 0}, udp_endpoint_from_native(address, length), data);
}

template <class NativeAddress, class NativeLength>
requires (!std::is_same_v<std::remove_cvref_t<NativeAddress>, UdpEndpoint> &&
          std::is_trivially_copyable_v<std::remove_cvref_t<NativeAddress>> &&
          std::is_integral_v<std::remove_cvref_t<NativeLength>>)
inline int recv_datagram(UdpNativeHandle handle, std::span<std::uint8_t> data,
                         NativeAddress &source, NativeLength &source_length, int timeout_ms)
{
    UdpEndpoint endpoint{};
    const int result = recv_datagram(UdpSocket{handle, 0}, data, endpoint, timeout_ms);
    if (result < 0 || !endpoint.valid() || endpoint.native_size > sizeof(source)) {
        source_length = 0;
        return result;
    }
    std::memset(&source, 0, sizeof(source));
    std::memcpy(&source, endpoint.native.data(), endpoint.native_size);
    source_length = static_cast<NativeLength>(endpoint.native_size);
    return result;
}

}
