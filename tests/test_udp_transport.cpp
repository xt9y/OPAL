#include <opal/udp_transport.hpp>

#include <array>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <span>
#include <string>
#include <vector>

int main()
{
    assert(opal::classify_udp_send_result(4, 4, 0) == opal::UdpSendResult::Sent);
    assert(opal::classify_udp_send_result(-1, 4, EAGAIN) == opal::UdpSendResult::WouldBlock);
    assert(opal::classify_udp_send_result(-1, 4, EWOULDBLOCK) == opal::UdpSendResult::WouldBlock);
    assert(opal::classify_udp_send_result(-1, 4, ENOBUFS) == opal::UdpSendResult::WouldBlock);
    assert(opal::classify_udp_send_result(-1, 4, ECONNREFUSED) == opal::UdpSendResult::Fatal);
    assert(opal::classify_udp_send_result(3, 4, 0) == opal::UdpSendResult::Fatal);

    auto a = opal::open_udp_socket();
    auto b = opal::open_udp_socket();
    assert(a.valid() && a.local_port > 0);
    assert(b.valid() && b.local_port > 0);

    opal::UdpEndpoint dst{};
    assert(opal::resolve_udp_endpoint("::1", b.local_port, dst));
    std::string numeric_host;
    std::uint16_t numeric_port = 0;
    assert(opal::udp_endpoint_numeric(dst, numeric_host, numeric_port));
    assert(numeric_port == b.local_port);

    const std::vector<std::uint8_t> payload = {'O', 'P', 'A', 'L'};
    assert(opal::send_datagram_result(a, dst, payload) == opal::UdpSendResult::Sent);
    assert(opal::send_datagram(a, dst, payload));

    std::uint8_t receive[32]{};
    opal::UdpEndpoint source{};
    int n = opal::recv_datagram(b, receive, source, 500);
    assert(n == 4 && std::memcmp(receive, payload.data(), 4) == 0);
    n = opal::recv_datagram(b, receive, source, 500);
    assert(n == 4 && std::memcmp(receive, payload.data(), 4) == 0);

    std::array<std::array<std::uint8_t, 4>, 8> send_batch{};
    std::array<std::span<const std::uint8_t>, 8> send_views{};
    for (std::size_t i = 0; i < send_batch.size(); ++i) {
        send_batch[i] = {'S', 'M', 'M', static_cast<std::uint8_t>(i)};
        send_views[i] = send_batch[i];
    }
    const auto send_result = opal::send_datagrams_batch(a, dst, send_views);
    assert(send_result.result == opal::UdpSendResult::Sent);
    assert(send_result.sent == send_views.size());

    std::array<std::array<std::uint8_t, 32>, 16> batch_buffers{};
    std::array<opal::UdpReceiveSlot, 16> slots{};
    for (std::size_t i = 0; i < slots.size(); ++i) slots[i].buffer = batch_buffers[i];
    std::size_t received_batch = 0;
    while (received_batch < send_views.size()) {
        const int got = opal::recv_datagrams_batch(b, slots, 500);
        assert(got > 0);
        for (int i = 0; i < got; ++i) {
            const auto index = received_batch + static_cast<std::size_t>(i);
            assert(index < send_views.size());
            const auto &slot = slots[static_cast<std::size_t>(i)];
            assert(slot.size == 4 && slot.buffer[0] == 'S' && slot.buffer[1] == 'M' && slot.buffer[2] == 'M');
            assert(slot.buffer[3] == static_cast<std::uint8_t>(index));
            assert(slot.source.valid());
        }
        received_batch += static_cast<std::size_t>(got);
    }

    opal::UdpEndpoint mapped_v4{};
    assert(opal::resolve_udp_endpoint("127.0.0.1", b.local_port, mapped_v4));
    assert(opal::udp_endpoint_numeric(mapped_v4, numeric_host, numeric_port));
    assert(numeric_port == b.local_port);

    for (std::uint8_t i = 0; i < 12; ++i) {
        const std::array<std::uint8_t, 3> p = {'B', 'T', i};
        assert(opal::send_datagram(a, dst, p));
    }
    const int batched = opal::recv_datagrams_batch(b, slots, 500);
    assert(batched >= 1 && batched <= 12);
    for (int i = 0; i < batched; ++i) {
        assert(slots[static_cast<std::size_t>(i)].size == 3);
        assert(slots[static_cast<std::size_t>(i)].buffer[0] == 'B');
        assert(slots[static_cast<std::size_t>(i)].buffer[1] == 'T');
        assert(slots[static_cast<std::size_t>(i)].source.valid());
    }

    const auto locals = opal::local_udp_candidates(a);
    for (const auto &candidate : locals) {
        assert(candidate.port == a.local_port);
        assert(candidate.host != "127.0.0.1" && candidate.host != "::1");
        assert(!candidate.host.empty());
    }

    assert(opal::send_datagram_result(a, dst, std::span<const std::uint8_t>{}) == opal::UdpSendResult::Fatal);
    assert(!opal::send_datagram(a, dst, std::span<const std::uint8_t>{}));
    const std::array<std::span<const std::uint8_t>, 1> empty_batch = {std::span<const std::uint8_t>{}};
    assert(opal::send_datagrams_batch(a, dst, empty_batch).result == opal::UdpSendResult::Fatal);
    opal::UdpSocket invalid{};
    assert(opal::send_datagrams_batch(invalid, dst, send_views).result == opal::UdpSendResult::Fatal);
    opal::UdpEndpoint none{};
    assert(!opal::resolve_udp_endpoint("", 1234, none));
    assert(!opal::resolve_udp_endpoint("::1", 0, none));

    opal::close_udp_socket(a);
    opal::close_udp_socket(b);
    assert(!a.valid() && !b.valid());
    return 0;
}
