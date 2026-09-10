#pragma once

#include <opal/display_backend.hpp>

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
    bool healthy() const;
    void stop();

    const DisplayTarget& target() const noexcept { return target_; }
    std::string backend_name() const;
    PlatformError last_platform_error() const;

private:
    std::unique_ptr<DisplayBackend> backend_;
    DisplayTarget target_{};
    PlatformError error_{};
    bool active_ = false;
};

ActiveHostDisplay active_host_display();

}
