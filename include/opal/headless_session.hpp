#pragma once

#include <opal/display_backend.hpp>

#include <memory>
#include <string>
#include <vector>

namespace opal {

class HeadlessSession {
public:
    HeadlessSession();
    HeadlessSession(const HeadlessSession&) = delete;
    HeadlessSession& operator=(const HeadlessSession&) = delete;
    ~HeadlessSession();

    bool start(const DisplayMode& mode);
    bool running() const;
    void stop();

    std::string wayland_display() const;
    std::string last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
