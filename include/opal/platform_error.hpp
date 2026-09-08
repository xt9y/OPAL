#pragma once

#include <cstdint>
#include <string>
#include <string_view>

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

constexpr std::string_view platform_component_name(PlatformComponent component) noexcept
{
    switch (component) {
        case PlatformComponent::Capture: return "capture";
        case PlatformComponent::Encoder: return "encoder";
        case PlatformComponent::Decoder: return "decoder";
        case PlatformComponent::Input: return "input";
        case PlatformComponent::Clipboard: return "clipboard";
        case PlatformComponent::AudioCapture: return "audio-capture";
        case PlatformComponent::Datagram: return "datagram";
        case PlatformComponent::Presenter: return "presenter";
        case PlatformComponent::System: return "system";
    }
    return "system";
}

constexpr std::string_view platform_failure_name(PlatformFailure failure) noexcept
{
    switch (failure) {
        case PlatformFailure::None: return "none";
        case PlatformFailure::Unsupported: return "unsupported";
        case PlatformFailure::PermissionDenied: return "permission-denied";
        case PlatformFailure::Unavailable: return "unavailable";
        case PlatformFailure::InvalidState: return "invalid-state";
        case PlatformFailure::DependencyMissing: return "dependency-missing";
        case PlatformFailure::OsError: return "os-error";
    }
    return "os-error";
}

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
