#pragma once

#include <opal/display_backend.hpp>

#include <atomic>
#include <chrono>
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
    std::string backend_name() const;
    PlatformError last_platform_error() const;

private:
    void start_duplicate_hotplug_watch();
    void stop_duplicate_hotplug_watch();

    std::unique_ptr<DisplayBackend> backend_;
    DisplayTarget target_{};
    PlatformError error_{};
    HostDisplayMode requested_mode_ = HostDisplayMode::Duplicate;
    std::atomic<bool> duplicate_watch_running_{false};
    std::atomic<bool> physical_display_usable_{false};
    std::thread duplicate_watch_thread_;
    bool duplicate_layout_active_ = false;
    bool active_ = false;
    std::chrono::steady_clock::time_point next_layout_check_{};
};

ActiveHostDisplay active_host_display();
void request_virtual_display_fallback() noexcept;

}
