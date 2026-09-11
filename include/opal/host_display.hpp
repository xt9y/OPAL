#pragma once

#include <opal/display_backend.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace opal {

struct ActiveHostDisplay {
    DisplayMode mode{};
    DisplayKind kind = DisplayKind::Physical;
    bool valid = false;
    std::string backend;
    std::string name;
};

class HostDisplayManager {
public:
    HostDisplayManager();
    HostDisplayManager(const HostDisplayManager&) = delete;
    HostDisplayManager& operator=(const HostDisplayManager&) = delete;
    ~HostDisplayManager();

    bool prepare(const StreamOptions& stream);
    bool healthy();
    void stop();

    const DisplayTarget& target() const noexcept { return target_; }
    bool physical_reselect_ready() const noexcept
    {
        return physical_reselect_ready_.load(std::memory_order_acquire);
    }
    std::string backend_name() const;
    PlatformError last_platform_error() const;

private:
    void start_physical_handoff_watch();
    void stop_physical_handoff_watch();

    std::unique_ptr<DisplayBackend> backend_;
    DisplayTarget target_{};
    PlatformError error_{};
    HostDisplayMode requested_mode_ = HostDisplayMode::Duplicate;
    std::atomic<bool> handoff_watch_running_{false};
    std::atomic<bool> physical_reselect_ready_{false};
    std::thread handoff_watch_thread_;
    bool active_ = false;
};

ActiveHostDisplay active_host_display();
void request_virtual_display_fallback() noexcept;

}
