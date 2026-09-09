#pragma once

#include <cstdint>
#include <string_view>

namespace opal {

enum class PlatformKind : std::uint8_t {
    Linux,
    MacOS,
    Windows,
    Unsupported
};

constexpr PlatformKind current_platform() noexcept
{
#if defined(__linux__)
    return PlatformKind::Linux;
#elif defined(__APPLE__)
    return PlatformKind::MacOS;
#elif defined(_WIN32)
    return PlatformKind::Windows;
#else
    return PlatformKind::Unsupported;
#endif
}

constexpr std::string_view platform_name(PlatformKind platform) noexcept
{
    switch (platform) {
        case PlatformKind::Linux: return "linux";
        case PlatformKind::MacOS: return "macos";
        case PlatformKind::Windows: return "windows";
        default: return "unsupported";
    }
}

}
