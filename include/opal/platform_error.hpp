#pragma once

#include <cstdint>
#include <string>

namespace opal {

enum class PlatformComponent : std::uint8_t {
    Capture,
    Encoder,
    Decoder,
    Input,
    Clipboard,
    AudioCapture,
    Datagram,
    Presenter,
    System
};

enum class PlatformFailure : std::uint8_t {
    None,
    Unsupported,
    PermissionDenied,
    Unavailable,
    InvalidState,
    DependencyMissing,
    OsError
};

struct PlatformError {
    PlatformComponent component = PlatformComponent::System;
    PlatformFailure failure = PlatformFailure::None;
    std::string message;
    bool fallback_possible = false;

    explicit operator bool() const noexcept
    {
        return failure != PlatformFailure::None;
    }
};

}
