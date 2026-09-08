#pragma once

#include <cstdint>
#include <string_view>

namespace opal {

enum class PlatformKind : std::uint8_t {
    Linux,
    MacOS,
    Unsupported
};

constexpr PlatformKind current_platform() noexcept
{
#if defined(__linux__)
    return PlatformKind::Linux;
#elif defined(__APPLE__)
    return PlatformKind::MacOS;
#else
    return PlatformKind::Unsupported;
#endif
}

constexpr std::string_view platform_name(PlatformKind platform) noexcept
{
    switch (platform) {
        case PlatformKind::Linux: return "linux";
        case PlatformKind::MacOS: return "macos";
        default: return "unsupported";
    }
}

}
