#include <opal/host_display.hpp>

#include <algorithm>
#include <mutex>
#include <utility>

namespace opal {
namespace {
std::mutex active_display_mu;
ActiveHostDisplay active_display_state;

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
    if (backend_->probe(target)) {
        target_ = std::move(target);
        active_ = true;
        publish_display(target_, backend_->backend_name());
        return true;
    }

    const auto mode = display_mode_for_stream(stream);
    if (!backend_->ensure(mode, target)) {
        error_ = backend_->last_platform_error();
        if (!error_) {
            error_ = {PlatformComponent::Capture, PlatformFailure::Unavailable,
                      "could not create a usable host display", false};
        }
        return false;
    }

    target_ = std::move(target);
    active_ = true;
    publish_display(target_, backend_->backend_name());
    return true;
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

}
