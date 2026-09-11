#include <opal/display_policy.hpp>
#include <opal/host_display.hpp>
#if defined(_WIN32)
#include <opal/windows_idd.hpp>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace opal {
namespace {
std::mutex active_display_mu;
ActiveHostDisplay active_display_state;
std::atomic<bool> force_virtual_once{false};

constexpr int kPhysicalHandoffStableSamples = 3;
constexpr auto kPhysicalHandoffPollInterval = std::chrono::milliseconds(100);

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

void HostDisplayManager::start_physical_handoff_watch()
{
    stop_physical_handoff_watch();
    physical_reselect_ready_.store(false, std::memory_order_release);
    if (!active_ || !backend_ || !target_.virtual_display() ||
        requested_mode_ != HostDisplayMode::Duplicate)
        return;

    handoff_watch_running_.store(true, std::memory_order_release);
    handoff_watch_thread_ = std::thread([this] {
        int stable_samples = 0;
        while (handoff_watch_running_.load(std::memory_order_acquire)) {
#if defined(_WIN32)
            const bool physical_available = windows_physical_display_usable();
#else
            const bool physical_available = backend_->physical_display_available(target_);
#endif
            if (should_reselect_physical(requested_mode_, target_.virtual_display(),
                                         physical_available)) {
                ++stable_samples;
                if (stable_samples >= kPhysicalHandoffStableSamples) {
                    physical_reselect_ready_.store(true, std::memory_order_release);
                    break;
                }
            } else {
                stable_samples = 0;
            }

            const auto deadline = std::chrono::steady_clock::now() + kPhysicalHandoffPollInterval;
            while (handoff_watch_running_.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        handoff_watch_running_.store(false, std::memory_order_release);
    });
}

void HostDisplayManager::stop_physical_handoff_watch()
{
    handoff_watch_running_.store(false, std::memory_order_release);
    if (handoff_watch_thread_.joinable()) handoff_watch_thread_.join();
}

bool HostDisplayManager::prepare(const StreamOptions& stream)
{
    const bool force_virtual = force_virtual_once.exchange(false, std::memory_order_acq_rel);
    stop();
    requested_mode_ = stream.host_display_mode;

    if (!backend_) {
        error_ = {PlatformComponent::Capture, PlatformFailure::Unavailable,
                  "display backend unavailable", false};
        return false;
    }

    DisplayTarget target;
    DisplayMode physical_mode{};
    bool physical_available = false;

    // Duplicate is a capture policy, not a request to renegotiate the host's
    // display mode. When a real desktop is available, capture it exactly as it
    // is. Extend deliberately skips the physical probe and creates/reuses an
    // OPAL virtual output instead.
    if (!force_virtual && stream.host_display_mode == HostDisplayMode::Duplicate &&
        backend_->probe(target)) {
        if (!target.virtual_display()) {
            physical_available = true;
            physical_mode = target.mode;
            if (capture_physical_for_topology(stream.host_display_mode,
                                              physical_available, force_virtual)) {
                adopt_display(std::move(target), backend_, target_, active_);
                start_physical_handoff_watch();
                return true;
            }
        } else {
            // A headless host may already have a persistent OPAL virtual output.
            // Reuse it unchanged rather than forcing a new mode onto it here.
            adopt_display(std::move(target), backend_, target_, active_);
            start_physical_handoff_watch();
            return true;
        }
    }

    const auto mode = virtual_mode_for_topology(stream.host_display_mode,
                                                physical_mode,
                                                display_mode_for_stream(stream),
                                                physical_available);
    if (backend_->ensure(mode, target)) {
        adopt_display(std::move(target), backend_, target_, active_);
        start_physical_handoff_watch();
        return true;
    }

    error_ = backend_->last_platform_error();
    if (!error_) {
        error_ = {PlatformComponent::Capture, PlatformFailure::Unavailable,
                  stream.host_display_mode == HostDisplayMode::Extend
                      ? "could not create the requested extended virtual host display"
                      : "could not create a usable host display",
                  false};
    }
    return false;
}

bool HostDisplayManager::healthy()
{
    return active_ && backend_ && backend_->healthy(target_);
}

void HostDisplayManager::stop()
{
    stop_physical_handoff_watch();
#if defined(_WIN32)
    // The Windows host daemon owns the IDD monitor, not an individual media
    // session. Keep OPAL's virtual output alive across reconnects, but never
    // modify or retain ownership of a physical display configuration.
    if (backend_ && active_ && !target_.virtual_display()) backend_->release(target_);
#else
    if (backend_ && active_) backend_->release(target_);
#endif
    target_ = {};
    error_ = {};
    requested_mode_ = HostDisplayMode::Duplicate;
    physical_reselect_ready_.store(false, std::memory_order_release);
    active_ = false;
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
