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
LONG windows_apply_duplicate_layout()
{
    constexpr UINT32 flags = SDC_APPLY | SDC_TOPOLOGY_CLONE |
                             SDC_ALLOW_CHANGES | SDC_PATH_PERSIST_IF_REQUIRED;
    return SetDisplayConfig(0, nullptr, 0, nullptr, flags);
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
    const bool prefer_virtual = force_virtual_once.exchange(false, std::memory_order_acq_rel);
    stop();
    if (!backend_) {
        error_ = {PlatformComponent::Capture, PlatformFailure::Unavailable,
                  "display backend unavailable", false};
        return false;
    }

    DisplayTarget target;

#if defined(_WIN32)
    auto mode = display_mode_for_stream(stream);
    bool physical_usable = false;

    // Duplicate is meaningful only when the physical desktop is genuinely
    // usable. Reuse the Windows backend's power/DDC/topology health probe that
    // drove the previously working headless fallback instead of trusting the
    // DISPLAY_DEVICE_ACTIVE bit, which can remain set while a monitor is off.
    // Extend deliberately skips this probe: it always requests an OPAL IDD.
    if (stream.host_display_mode == HostDisplayMode::Duplicate && !prefer_virtual) {
        DisplayTarget probed;
        if (backend_->probe(probed)) {
            if (!probed.virtual_display()) {
                physical_usable = true;
                mode = probed.mode;
                backend_->release(probed);
            }
            // A virtual probe found the persistent OPAL IDD from the previous
            // media session. Do not release it here: WindowsDisplayBackend::release
            // sends DestroyMonitor, and rapid departure/arrival cycles can leave
            // the UMDF/IddCx device unavailable on the next connection.
        }
    }

    PlatformError virtual_error{};
    if (backend_->ensure(mode, target)) {
        // The proven duplicate path is the only topology change that belongs
        // here. For extend and headless duplicate, creating/reusing the IDD
        // monitor is enough; the Windows IDD capture backend performs attachment
        // after the monitor has actually arrived.
        if (!(stream.host_display_mode == HostDisplayMode::Duplicate && physical_usable)) {
            adopt_display(std::move(target), backend_, target_, active_);
            return true;
        }

        const LONG topology_result = windows_apply_duplicate_layout();
        if (topology_result == ERROR_SUCCESS) {
            adopt_display(std::move(target), backend_, target_, active_);
            return true;
        }
        backend_->release(target);
        target = {};
        virtual_error = {PlatformComponent::Capture, PlatformFailure::OsError,
                         "could not apply Windows duplicate display topology (Win32 " +
                             std::to_string(topology_result) + ")",
                         false};
    } else {
        virtual_error = backend_->last_platform_error();
    }

    // If cloning a confirmed-live physical display cannot be established,
    // retain the old availability fallback and capture that physical display.
    // Never take this path for a forced/headless virtual fallback.
    if (stream.host_display_mode == HostDisplayMode::Duplicate &&
        physical_usable && !prefer_virtual && backend_->probe(target) && !target.virtual_display()) {
        error_ = {};
        adopt_display(std::move(target), backend_, target_, active_);
        return true;
    }

    if (target.virtual_display()) backend_->release(target);
    error_ = virtual_error;
    if (!error_) error_ = backend_->last_platform_error();
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
#if defined(_WIN32)
    // The host daemon owns the IDD monitor, not an individual media session.
    // Keep virtual targets alive across disconnect/restart so repeated client
    // sessions do not churn IddCx monitor departure/arrival. A later explicit
    // host-lifecycle cleanup can remove the persistent monitor once.
    if (backend_ && active_ && !target_.virtual_display()) backend_->release(target_);
#else
    if (backend_ && active_) backend_->release(target_);
#endif
    target_ = {};
    error_ = {};
    active_ = false;
#if defined(_WIN32)
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
