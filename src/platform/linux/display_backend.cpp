#include <opal/display_backend.hpp>
#include <opal/headless_session.hpp>
#include <opal/kwin_virtual_display.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <unistd.h>

namespace opal {
namespace {

bool environment_value(const char* name)
{
    const char* value = std::getenv(name);
    return value && *value;
}

bool wayland_socket_available()
{
    const char* display = std::getenv("WAYLAND_DISPLAY");
    if (!display || !*display) return false;
    std::filesystem::path socket(display);
    if (!socket.is_absolute()) {
        const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        if (!runtime || !*runtime) return false;
        socket = std::filesystem::path(runtime) / socket;
    }
    std::error_code error;
    return std::filesystem::exists(socket, error) && !error;
}

void enable_virtual_input(const DisplayMode& mode)
{
    setenv("OPAL_KWIN_VIRTUAL_INPUT", "1", 1);
    const auto width = std::to_string(mode.width);
    const auto height = std::to_string(mode.height);
    setenv("OPAL_KWIN_VIRTUAL_WIDTH", width.c_str(), 1);
    setenv("OPAL_KWIN_VIRTUAL_HEIGHT", height.c_str(), 1);
}

void disable_virtual_input()
{
    unsetenv("OPAL_KWIN_VIRTUAL_INPUT");
    unsetenv("OPAL_KWIN_VIRTUAL_WIDTH");
    unsetenv("OPAL_KWIN_VIRTUAL_HEIGHT");
}

class LinuxDisplayBackend final : public DisplayBackend {
public:
    ~LinuxDisplayBackend() override { reset(); }

    bool probe(DisplayTarget& target) override
    {
        reset();
        error_ = {};

        if (wayland_socket_available()) {
            kwin_ = std::make_unique<KwinVirtualDisplay>();
            if (kwin_->connect() && kwin_->has_output()) {
                kwin_->close();
                kwin_.reset();
                target = {};
                target.kind = DisplayKind::Physical;
                target.capture_kind = DisplayCaptureKind::Desktop;
                target.name = "Wayland desktop";
                backend_ = "wayland-existing";
                return true;
            }
            kwin_.reset();
            if (!environment_value("KDE_FULL_SESSION") && !environment_value("KDE_SESSION_VERSION")) {
                target = {};
                target.kind = DisplayKind::Physical;
                target.capture_kind = DisplayCaptureKind::Desktop;
                target.name = "Wayland desktop";
                backend_ = "wayland-existing";
                return true;
            }
            return false;
        }

        if (environment_value("DISPLAY")) {
            target = {};
            target.kind = DisplayKind::Physical;
            target.capture_kind = DisplayCaptureKind::Desktop;
            target.name = "X11 desktop";
            backend_ = "x11-existing";
            return true;
        }
        return false;
    }

    bool ensure(const DisplayMode& mode, DisplayTarget& target) override
    {
        reset();
        error_ = {};

        if (wayland_socket_available()) {
            kwin_ = std::make_unique<KwinVirtualDisplay>();
            if (!kwin_->connect()) return fail(kwin_->last_error());
            std::uint32_t node = 0;
            if (!kwin_->create_virtual_output("OPAL-1", mode.width, mode.height, mode.scale, node))
                return fail(kwin_->last_error());

            target = {};
            target.kind = DisplayKind::VirtualExistingSession;
            target.capture_kind = DisplayCaptureKind::PipeWireNode;
            target.mode = mode;
            target.name = "OPAL-1";
            target.pipewire_node = node;
            target.owned_by_opal = true;
            backend_ = "kwin-virtual-output";
            enable_virtual_input(mode);
            virtual_input_enabled_ = true;
            return true;
        }

        session_ = std::make_unique<HeadlessSession>();
        if (!session_->start(mode)) return fail(session_->last_error());
        if (!connect_managed_kwin()) return fail("managed KWin started but its screencast interface did not become ready");

        std::uint32_t node = 0;
        bool streamed = kwin_->stream_existing_output(node);
        if (!streamed)
            streamed = kwin_->create_virtual_output("OPAL-1", mode.width, mode.height, mode.scale, node);
        if (!streamed) return fail(kwin_->last_error());

        target = {};
        target.kind = DisplayKind::VirtualManagedSession;
        target.capture_kind = DisplayCaptureKind::PipeWireNode;
        target.mode = mode;
        target.name = "OPAL Managed Plasma";
        target.pipewire_node = node;
        target.owned_by_opal = true;
        backend_ = "kwin-managed-plasma";
        enable_virtual_input(mode);
        virtual_input_enabled_ = true;
        return true;
    }

    bool reconfigure(const DisplayMode& mode, DisplayTarget& target) override
    {
        if (!target.virtual_display()) return true;
        const DisplayKind previous = target.kind;
        reset();
        target = {};

        if (previous == DisplayKind::VirtualManagedSession) {
            session_ = std::make_unique<HeadlessSession>();
            if (!session_->start(mode)) return fail(session_->last_error());
            if (!connect_managed_kwin()) return fail("managed KWin screencast interface did not become ready");
            std::uint32_t node = 0;
            if (!kwin_->stream_existing_output(node)) return fail(kwin_->last_error());
            target.kind = DisplayKind::VirtualManagedSession;
            target.capture_kind = DisplayCaptureKind::PipeWireNode;
            target.mode = mode;
            target.name = "OPAL Managed Plasma";
            target.pipewire_node = node;
            target.owned_by_opal = true;
            backend_ = "kwin-managed-plasma";
            enable_virtual_input(mode);
            virtual_input_enabled_ = true;
            return true;
        }

        kwin_ = std::make_unique<KwinVirtualDisplay>();
        if (!kwin_->connect()) return fail(kwin_->last_error());
        std::uint32_t node = 0;
        if (!kwin_->create_virtual_output("OPAL-1", mode.width, mode.height, mode.scale, node))
            return fail(kwin_->last_error());
        target.kind = DisplayKind::VirtualExistingSession;
        target.capture_kind = DisplayCaptureKind::PipeWireNode;
        target.mode = mode;
        target.name = "OPAL-1";
        target.pipewire_node = node;
        target.owned_by_opal = true;
        backend_ = "kwin-virtual-output";
        enable_virtual_input(mode);
        virtual_input_enabled_ = true;
        return true;
    }

    bool healthy(const DisplayTarget& target) override
    {
        if (target.kind == DisplayKind::Physical)
            return wayland_socket_available() || environment_value("DISPLAY");
        if (!kwin_ || target.pipewire_node == 0) return false;
        if (target.kind == DisplayKind::VirtualManagedSession && (!session_ || !session_->running())) return false;
        return true;
    }

    void release(DisplayTarget& target) override
    {
        reset();
        target = {};
    }

    std::string backend_name() const override { return backend_; }
    PlatformError last_platform_error() const override { return error_; }

private:
    bool connect_managed_kwin()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        std::string last_error;
        do {
            kwin_ = std::make_unique<KwinVirtualDisplay>();
            if (kwin_->connect()) return true;
            last_error = kwin_->last_error();
            kwin_.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } while (std::chrono::steady_clock::now() < deadline);
        if (!last_error.empty())
            error_ = {PlatformComponent::Capture, PlatformFailure::Unavailable, last_error, false};
        return false;
    }

    bool fail(std::string message)
    {
        if (!error_)
            error_ = {PlatformComponent::Capture, PlatformFailure::Unavailable,
                      message.empty() ? "Linux headless display creation failed" : std::move(message), false};
        reset_resources();
        return false;
    }

    void reset_resources()
    {
        if (virtual_input_enabled_) {
            disable_virtual_input();
            virtual_input_enabled_ = false;
        }
        if (kwin_) kwin_->close();
        kwin_.reset();
        if (session_) session_->stop();
        session_.reset();
    }

    void reset()
    {
        reset_resources();
        backend_ = "linux-display";
    }

    std::unique_ptr<KwinVirtualDisplay> kwin_;
    std::unique_ptr<HeadlessSession> session_;
    std::string backend_ = "linux-display";
    PlatformError error_{};
    bool virtual_input_enabled_ = false;
};

}

std::unique_ptr<DisplayBackend> make_display_backend()
{
    return std::make_unique<LinuxDisplayBackend>();
}

}
