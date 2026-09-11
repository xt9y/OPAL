#pragma once

#include <opal/display_backend.hpp>

#include <chrono>
#include <memory>
#include <string>

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
    std::unique_ptr<DisplayBackend> backend_;
    DisplayTarget target_{};
    PlatformError error_{};
    HostDisplayMode requested_mode_ = HostDisplayMode::Duplicate;
    bool duplicate_layout_active_ = false;
    bool active_ = false;
    std::chrono::steady_clock::time_point next_layout_check_{};
};

ActiveHostDisplay active_host_display();
void request_virtual_display_fallback() noexcept;

}
