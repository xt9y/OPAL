#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace opal {

constexpr int kUdpQueueBufferBytes = 64 * 1024;
constexpr int kUdpReceiveQueueBufferBytes = 256 * 1024;
constexpr int kUdpInteractiveTrafficClass = 0xb8;
constexpr std::size_t kUdpReceiveBatchMax = 32;
constexpr std::size_t kUdpSendBatchMax = 32;

enum class UdpSendResult : std::uint8_t {
    Sent = 0,
    WouldBlock,
    Fatal
};

struct UdpSendBatchResult {
    std::size_t sent = 0;
    UdpSendResult result = UdpSendResult::Fatal;
};

struct UdpCandidate {
    std::string host;
    std::uint16_t port = 0;
};

using UdpNativeHandle = std::uintptr_t;
inline constexpr UdpNativeHandle kInvalidUdpHandle = std::numeric_limits<UdpNativeHandle>::max();

struct UdpSocket {
    UdpNativeHandle handle = kInvalidUdpHandle;
    std::uint16_t local_port = 0;

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

}
