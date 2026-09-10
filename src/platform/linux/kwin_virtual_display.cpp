#include <opal/kwin_virtual_display.hpp>

#include "zkde-screencast-client-protocol.h"

#include <wayland-client.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <string>
#include <vector>

namespace opal {
namespace {

using Clock = std::chrono::steady_clock;

}

struct KwinVirtualDisplay::Impl {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    zkde_screencast_unstable_v1* screencast = nullptr;
    zkde_screencast_stream_unstable_v1* stream = nullptr;
    std::vector<wl_output*> outputs;
    std::uint32_t node = 0;
    bool complete = false;
    bool failed = false;
    std::string error;

    static void registry_global(void* data, wl_registry* registry, std::uint32_t name,
                                const char* interface, std::uint32_t version)
    {
        auto& self = *static_cast<Impl*>(data);
        if (std::strcmp(interface, zkde_screencast_unstable_v1_interface.name) == 0) {
            if (!self.screencast) {
                const std::uint32_t bind_version = std::min<std::uint32_t>(version, 5);
                self.screencast = static_cast<zkde_screencast_unstable_v1*>(
                    wl_registry_bind(registry, name, &zkde_screencast_unstable_v1_interface, bind_version));
            }
            return;
        }
        if (std::strcmp(interface, wl_output_interface.name) == 0) {
            auto* output = static_cast<wl_output*>(wl_registry_bind(
                registry, name, &wl_output_interface, std::min<std::uint32_t>(version, 2)));
            if (output) self.outputs.push_back(output);
        }
    }

    static void registry_remove(void*, wl_registry*, std::uint32_t) {}

    static void stream_closed(void* data, zkde_screencast_stream_unstable_v1*)
    {
        auto& self = *static_cast<Impl*>(data);
        self.failed = true;
        self.complete = true;
        if (self.error.empty()) self.error = "KWin closed the screencast stream";
    }

    static void stream_created(void* data, zkde_screencast_stream_unstable_v1*, std::uint32_t node)
    {
        auto& self = *static_cast<Impl*>(data);
        self.node = node;
        self.complete = true;
    }

    static void stream_failed(void* data, zkde_screencast_stream_unstable_v1*, const char* message)
    {
        auto& self = *static_cast<Impl*>(data);
        self.failed = true;
        self.complete = true;
        self.error = message && *message ? message : "KWin screencast request failed";
    }

    bool wait_created(int timeout_ms)
    {
        if (!display || !stream) return false;
        complete = false;
        failed = false;
        node = 0;

        static const zkde_screencast_stream_unstable_v1_listener listener = {
            &Impl::stream_closed,
            &Impl::stream_created,
            &Impl::stream_failed,
        };
        if (zkde_screencast_stream_unstable_v1_add_listener(stream, &listener, this) != 0) {
            error = "could not attach KWin screencast listener";
            return false;
        }

        if (wl_display_flush(display) < 0 && errno != EAGAIN) {
            error = "could not flush KWin Wayland request";
            return false;
        }

        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!complete) {
            if (wl_display_dispatch_pending(display) < 0) {
                error = "KWin Wayland connection failed";
                return false;
            }
            if (complete) break;

            const auto now = Clock::now();
            if (now >= deadline) {
                error = "KWin virtual display creation timed out";
                return false;
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            pollfd fd{wl_display_get_fd(display), POLLIN, 0};
            const int rc = poll(&fd, 1, static_cast<int>(std::min<long long>(remaining, 100)));
            if (rc < 0) {
                if (errno == EINTR) continue;
                error = "polling the KWin Wayland connection failed";
                return false;
            }
            if (rc == 0) continue;
            if ((fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                error = "KWin Wayland connection closed";
                return false;
            }
            if ((fd.revents & POLLIN) != 0 && wl_display_dispatch(display) < 0) {
                error = "KWin Wayland dispatch failed";
                return false;
            }
        }
        return !failed && node != 0;
    }

    void release_stream()
    {
        if (stream) {
            zkde_screencast_stream_unstable_v1_close(stream);
            stream = nullptr;
        }
        node = 0;
        complete = false;
        failed = false;
    }

    void reset()
    {
        release_stream();
        for (auto* output : outputs) if (output) wl_output_destroy(output);
        outputs.clear();
        if (screencast) {
            zkde_screencast_unstable_v1_destroy(screencast);
            screencast = nullptr;
        }
        if (registry) {
            wl_registry_destroy(registry);
            registry = nullptr;
        }
        if (display) {
            wl_display_disconnect(display);
            display = nullptr;
        }
    }
};

KwinVirtualDisplay::KwinVirtualDisplay() : impl_(std::make_unique<Impl>()) {}
KwinVirtualDisplay::~KwinVirtualDisplay() { close(); }

bool KwinVirtualDisplay::connect()
{
    close();
    impl_ = std::make_unique<Impl>();
    impl_->display = wl_display_connect(nullptr);
    if (!impl_->display) {
        impl_->error = "could not connect to the KWin Wayland display";
        return false;
    }
    impl_->registry = wl_display_get_registry(impl_->display);
    if (!impl_->registry) {
        impl_->error = "KWin Wayland registry is unavailable";
        return false;
    }
    static const wl_registry_listener listener = {
        &Impl::registry_global,
        &Impl::registry_remove,
    };
    if (wl_registry_add_listener(impl_->registry, &listener, impl_.get()) != 0 ||
        wl_display_roundtrip(impl_->display) < 0) {
        impl_->error = "could not enumerate KWin Wayland globals";
        return false;
    }
    if (!impl_->screencast) {
        impl_->error = "KWin does not expose zkde_screencast_unstable_v1";
        return false;
    }
    return true;
}

bool KwinVirtualDisplay::stream_existing_output(std::uint32_t& pipewire_node)
{
    pipewire_node = 0;
    if (!impl_ || !impl_->display || !impl_->screencast || impl_->outputs.empty()) {
        if (impl_) impl_->error = "KWin has no existing Wayland output to stream";
        return false;
    }
    impl_->release_stream();
    impl_->stream = zkde_screencast_unstable_v1_stream_output(
        impl_->screencast, impl_->outputs.front(), ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED);
    if (!impl_->stream || !impl_->wait_created(3000)) return false;
    pipewire_node = impl_->node;
    return true;
}

bool KwinVirtualDisplay::create_virtual_output(const std::string& name, int width, int height, float scale,
                                               std::uint32_t& pipewire_node)
{
    pipewire_node = 0;
    if (!impl_ || !impl_->display || !impl_->screencast) {
        if (impl_) impl_->error = "KWin screencast protocol is unavailable";
        return false;
    }
    impl_->release_stream();
    impl_->stream = zkde_screencast_unstable_v1_stream_virtual_output_with_description(
        impl_->screencast,
        name.c_str(),
        "OPAL headless remote desktop",
        width,
        height,
        wl_fixed_from_double(scale > 0.0f ? scale : 1.0f),
        ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED);
    if (!impl_->stream || !impl_->wait_created(3000)) return false;
    pipewire_node = impl_->node;
    return true;
}

void KwinVirtualDisplay::close()
{
    if (impl_) impl_->reset();
}

bool KwinVirtualDisplay::has_output() const
{
    return impl_ && !impl_->outputs.empty();
}

std::string KwinVirtualDisplay::last_error() const
{
    return impl_ ? impl_->error : "KWin display client unavailable";
}

}
