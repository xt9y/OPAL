#include <opal/host_display.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <mutex>
#include <string>
#include <utility>

namespace opal {
namespace {
std::mutex active_display_mu;
ActiveHostDisplay active_display_state;
std::atomic<bool> force_virtual_once{false};

void publish_display(const DisplayTarget& target, const std::string& backend)
{
    std::lock_guard<std::mutex> lock(active_display_mu);
    active_display_state.mode = target.mode;
    active_display_state.kind = target.kind;
    active_display_state.valid = true;
    active_display_state.backend = backend;
    active_display_state.name = target.name;
}

void clear_display()
{
    std::lock_guard<std::mutex> lock(active_display_mu);
    active_display_state = {};
}

void adopt_display(DisplayTarget&& target, std::unique_ptr<DisplayBackend>& backend,
                   DisplayTarget& active_target, bool& active)
{
    active_target = std::move(target);
    active = true;
    publish_display(active_target, backend->backend_name());
}

#if defined(_WIN32)
bool contains_case_insensitive(const wchar_t* value, const wchar_t* needle)
{
    if (!value || !needle || !*needle) return false;
    std::wstring haystack(value);
    std::wstring pattern(needle);
    std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towupper(c));
    });
    std::transform(pattern.begin(), pattern.end(), pattern.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towupper(c));
    });
    return haystack.find(pattern) != std::wstring::npos;
}

bool windows_opal_display(const DISPLAY_DEVICEW& device)
{
    return contains_case_insensitive(device.DeviceID, L"OPALDISPLAY") ||
           contains_case_insensitive(device.DeviceString, L"OPAL VIRTUAL DISPLAY");
}

bool windows_physical_display_mode(DisplayMode& mode)
{
    for (DWORD index = 0;; ++index) {
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        if (!EnumDisplayDevicesW(nullptr, index, &device, 0)) break;
        if ((device.StateFlags & DISPLAY_DEVICE_ACTIVE) == 0 ||
            (device.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) != 0 ||
            windows_opal_display(device))
            continue;

        DEVMODEW current{};
        current.dmSize = sizeof(current);
        if (EnumDisplaySettingsW(device.DeviceName, ENUM_CURRENT_SETTINGS, &current)) {
            if (current.dmPelsWidth > 0) mode.width = static_cast<int>(current.dmPelsWidth) & ~1;
            if (current.dmPelsHeight > 0) mode.height = static_cast<int>(current.dmPelsHeight) & ~1;
            if (current.dmDisplayFrequency > 1)
                mode.refresh_hz = std::clamp(static_cast<int>(current.dmDisplayFrequency), 30, 240);
        }
        return true;
    }
    return false;
}

LONG windows_apply_display_layout(HostDisplayMode requested, bool physical_active)
{
    const UINT32 topology = requested == HostDisplayMode::Duplicate && physical_active
        ? SDC_TOPOLOGY_CLONE : SDC_TOPOLOGY_EXTEND;
    constexpr UINT32 common = SDC_APPLY | SDC_ALLOW_CHANGES | SDC_PATH_PERSIST_IF_REQUIRED;
    return SetDisplayConfig(0, nullptr, 0, nullptr, common | topology);
}
#endif
}

DisplayMode display_mode_for_stream(const StreamOptions& stream)
{
    DisplayMode mode;
    mode.width = stream.max_width > 0 ? std::clamp(stream.max_width, 640, 7680) : 1920;
    mode.height = stream.max_height > 0 ? std::clamp(stream.max_height, 480, 4320) : 1080;
    mode.width &= ~1;
    mode.height &= ~1;
    mode.refresh_hz = std::clamp(stream.fps, 15, 240);
    mode.scale = 1.0f;
    return mode;
}

const char* display_kind_name(DisplayKind kind) noexcept
{
    switch (kind) {
        case DisplayKind::Physical: return "physical";
        case DisplayKind::VirtualExistingSession: return "virtual-existing-session";
        case DisplayKind::VirtualManagedSession: return "virtual-managed-session";
    }
    return "unknown";
}

HostDisplayManager::HostDisplayManager() : backend_(make_display_backend()) {}
HostDisplayManager::~HostDisplayManager() { stop(); }

bool HostDisplayManager::prepare(const StreamOptions& stream)
{
    stop();
    if (!backend_) {
        error_ = {PlatformComponent::Capture, PlatformFailure::Unavailable,
                  "display backend unavailable", false};
        return false;
    }

    DisplayTarget target;
    const bool prefer_virtual = force_virtual_once.exchange(false, std::memory_order_acq_rel);

#if defined(_WIN32)
    (void)prefer_virtual;
    auto mode = display_mode_for_stream(stream);
    const bool physical_active = windows_physical_display_mode(mode);

    if (backend_->ensure(mode, target)) {
        const LONG topology_result = windows_apply_display_layout(stream.host_display_mode, physical_active);
        if (topology_result == ERROR_SUCCESS) {
            adopt_display(std::move(target), backend_, target_, active_);
            return true;
        }
        backend_->release(target);
        target = {};
        error_ = {PlatformComponent::Capture, PlatformFailure::OsError,
                  std::string("could not apply Windows ") +
                      (stream.host_display_mode == HostDisplayMode::Duplicate ? "duplicate" : "extend") +
                      " display topology (Win32 " + std::to_string(topology_result) + ")",
                  false};
        return false;
    }

    error_ = backend_->last_platform_error();
    if (!error_) {
        error_ = {PlatformComponent::Capture, PlatformFailure::Unavailable,
                  "could not create the requested Windows virtual host display", false};
    }
    return false;
#else
    if (!prefer_virtual && backend_->probe(target)) {
        adopt_display(std::move(target), backend_, target_, active_);
        return true;
    }

    const auto mode = display_mode_for_stream(stream);
    if (backend_->ensure(mode, target)) {
        adopt_display(std::move(target), backend_, target_, active_);
        return true;
    }

    error_ = backend_->last_platform_error();
    if (!error_) {
        error_ = {PlatformComponent::Capture, PlatformFailure::Unavailable,
                  prefer_virtual ? "could not create the requested virtual host display"
                                 : "could not create a usable host display",
                  false};
    }
    return false;
#endif
}

bool HostDisplayManager::healthy() const
{
    return active_ && backend_ && backend_->healthy(target_);
}

void HostDisplayManager::stop()
{
    if (backend_ && active_) backend_->release(target_);
    target_ = {};
    error_ = {};
    active_ = false;
#if defined(_WIN32)
    // A virtual fallback belongs to one capture lifetime only. Do not let a
    // headless session force the next session back onto a stale IDD target
    // after a physical monitor has returned.
    force_virtual_once.store(false, std::memory_order_release);
#endif
    clear_display();
}

std::string HostDisplayManager::backend_name() const
{
    if (!backend_) return "unavailable";
    return backend_->backend_name();
}

PlatformError HostDisplayManager::last_platform_error() const
{
    if (error_) return error_;
    return backend_ ? backend_->last_platform_error() : PlatformError{};
}

ActiveHostDisplay active_host_display()
{
    std::lock_guard<std::mutex> lock(active_display_mu);
    return active_display_state;
}

void request_virtual_display_fallback() noexcept
{
    force_virtual_once.store(true, std::memory_order_release);
}

}
