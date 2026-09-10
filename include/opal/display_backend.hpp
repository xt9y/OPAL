#pragma once

#include <opal/media_profile.hpp>
#include <opal/platform_error.hpp>
#if defined(_WIN32)
#include <opal/windows_power_compat.hpp>
#endif

#include <cstdint>
#include <memory>
#include <string>

namespace opal {

struct DisplayMode {
    int width = 1920;
    int height = 1080;
    int refresh_hz = 60;
    float scale = 1.0f;
};

enum class DisplayKind {
    Physical,
    VirtualExistingSession,
    VirtualManagedSession
};

enum class DisplayCaptureKind {
    Desktop,
    NativeDisplay,
    PipeWireNode,
    WindowsIddSwapchain
};

struct DisplayTarget {
    DisplayKind kind = DisplayKind::Physical;
    DisplayCaptureKind capture_kind = DisplayCaptureKind::Desktop;
    DisplayMode mode{};
    std::string name;
    std::uint64_t native_id = 0;
    std::uint32_t pipewire_node = 0;
    std::uint64_t pipewire_serial = 0;
    bool owned_by_opal = false;

    bool virtual_display() const noexcept { return kind != DisplayKind::Physical; }
};

class DisplayBackend {
public:
    virtual ~DisplayBackend() = default;

    virtual bool probe(DisplayTarget& target) = 0;
    virtual bool ensure(const DisplayMode& mode, DisplayTarget& target) = 0;
    virtual bool reconfigure(const DisplayMode& mode, DisplayTarget& target) = 0;
    virtual bool healthy(const DisplayTarget& target) = 0;
    virtual void release(DisplayTarget& target) = 0;

    virtual std::string backend_name() const = 0;
    virtual PlatformError last_platform_error() const = 0;
};

std::unique_ptr<DisplayBackend> make_display_backend();
DisplayMode display_mode_for_stream(const StreamOptions& stream);
const char* display_kind_name(DisplayKind kind) noexcept;

}
