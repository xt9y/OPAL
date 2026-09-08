#include <opal/udp_batch_fallback.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <span>

int main()
{
    std::array<std::array<std::uint8_t, 2>, 4> storage{{{{1,1}},{{2,2}},{{3,3}},{{4,4}}}};
    std::array<std::span<const std::uint8_t>, 4> packets{};
    for (std::size_t i = 0; i < packets.size(); ++i) packets[i] = storage[i];

    std::size_t calls = 0;
    auto partial = opal::portable_udp_batch_send(packets, [&](std::span<const std::uint8_t>) {
        ++calls;
        return calls == 3 ? opal::UdpSendResult::WouldBlock : opal::UdpSendResult::Sent;
    });
    assert(partial.sent == 2);
    assert(partial.result == opal::UdpSendResult::WouldBlock);
    assert(calls == 3);

    calls = 0;
    auto fatal = opal::portable_udp_batch_send(packets, [&](std::span<const std::uint8_t>) {
        ++calls;
        return opal::UdpSendResult::Fatal;
    });
    assert(fatal.sent == 0);
    assert(fatal.result == opal::UdpSendResult::Fatal);
    assert(calls == 1);

    calls = 0;
    auto complete = opal::portable_udp_batch_send(packets, [&](std::span<const std::uint8_t>) {
        ++calls;
        return opal::UdpSendResult::Sent;
    });
    assert(complete.sent == packets.size());
    assert(complete.result == opal::UdpSendResult::Sent);
    assert(calls == packets.size());

    std::array<std::span<const std::uint8_t>, 1> empty{{std::span<const std::uint8_t>{}}};
    calls = 0;
    auto invalid = opal::portable_udp_batch_send(empty, [&](std::span<const std::uint8_t>) {
        ++calls;
        return opal::UdpSendResult::Sent;
    });
    assert(invalid.sent == 0);
    assert(invalid.result == opal::UdpSendResult::Fatal);
    assert(calls == 0);

    return 0;
}
