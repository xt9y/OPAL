#pragma once

#include <cstdint>
#include <memory>
#include <span>

namespace opal {

enum class NativeVideoFrameKind : std::uint8_t {
    Cpu,
    Opaque
};

struct NativeVideoFrame {
    NativeVideoFrameKind kind = NativeVideoFrameKind::Cpu;
    int width = 0;
    int height = 0;
    int stride = 0;
    std::uint32_t pixel_format = 0;
    std::uint64_t capture_time_us = 0;
    std::span<const std::uint8_t> bytes{};
    void *opaque = nullptr;
    std::shared_ptr<void> owner;

    bool valid() const noexcept
    {
        if (width <= 0 || height <= 0 || capture_time_us == 0) return false;
        return kind == NativeVideoFrameKind::Opaque ? opaque != nullptr : !bytes.empty();
    }
};

}
