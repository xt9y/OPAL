#include <opal/headless_session.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace opal {
namespace {

bool executable_on_path(const char* name)
{
    if (!name || !*name) return false;
    const char* path = std::getenv("PATH");
    if (!path || !*path) return false;
    std::stringstream stream(path);
    std::string directory;
    while (std::getline(stream, directory, ':')) {
        if (directory.empty()) directory = ".";
        if (::access((std::filesystem::path(directory) / name).c_str(), X_OK) == 0) return true;
    }
    return false;
}

std::string shell_quote(const std::string& value)
{
    std::string out = "'";
    for (const char c : value) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += '\'';
    return out;
}

struct SavedEnv {
    std::string name;
    std::optional<std::string> value;
};

SavedEnv save_env(const char* name)
{
    const char* value = std::getenv(name);
    return {name, value ? std::optional<std::string>(value) : std::nullopt};
}

void restore_env(const SavedEnv& saved)
{
    if (saved.value) ::setenv(saved.name.c_str(), saved.value->c_str(), 1);
    else ::unsetenv(saved.name.c_str());
}

}

struct HeadlessSession::Impl {
    pid_t leader = -1;
    std::string display_name = "opal-wayland-0";
    std::string runtime_dir;
    std::string error;
    bool environment_installed = false;
    std::vector<SavedEnv> saved_environment;

    std::filesystem::path socket_path() const
    {
        return std::filesystem::path(runtime_dir) / display_name;
    }

    bool save_and_install_environment()
    {
        static constexpr const char* names[] = {
            "XDG_RUNTIME_DIR", "WAYLAND_DISPLAY", "XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP",
            "XDG_SESSION_DESKTOP", "KDE_FULL_SESSION", "QT_QPA_PLATFORM", "DBUS_SESSION_BUS_ADDRESS"
        };
        saved_environment.clear();
        for (const char* name : names) saved_environment.push_back(save_env(name));
        environment_installed = true;

        if (::setenv("XDG_RUNTIME_DIR", runtime_dir.c_str(), 1) != 0 ||
            ::setenv("WAYLAND_DISPLAY", display_name.c_str(), 1) != 0 ||
            ::setenv("XDG_SESSION_TYPE", "wayland", 1) != 0 ||
            ::setenv("XDG_CURRENT_DESKTOP", "KDE", 1) != 0 ||
            ::setenv("XDG_SESSION_DESKTOP", "KDE", 1) != 0 ||
            ::setenv("KDE_FULL_SESSION", "true", 1) != 0 ||
            ::setenv("QT_QPA_PLATFORM", "wayland", 1) != 0) {
            error = "could not install managed Plasma environment";
            restore_environment();
            return false;
        }
        const auto bus = std::filesystem::path(runtime_dir) / "bus";
        if (std::filesystem::exists(bus)) {
            const std::string address = "unix:path=" + bus.string();
            if (::setenv("DBUS_SESSION_BUS_ADDRESS", address.c_str(), 1) != 0) {
                error = "could not select the user D-Bus session";
                restore_environment();
                return false;
            }
        }
        return true;
    }

    void restore_environment()
    {
        if (!environment_installed) return;
        for (auto it = saved_environment.rbegin(); it != saved_environment.rend(); ++it) restore_env(*it);
        saved_environment.clear();
        environment_installed = false;
    }
};

HeadlessSession::HeadlessSession() : impl_(std::make_unique<Impl>()) {}
HeadlessSession::~HeadlessSession() { stop(); }

bool HeadlessSession::start(const DisplayMode& requested)
{
    stop();
    impl_ = std::make_unique<Impl>();

    for (const char* required : {"kwin_wayland", "plasmashell", "Xwayland"}) {
        if (!executable_on_path(required)) {
            impl_->error = std::string("managed headless Plasma requires ") + required;
            return false;
        }
    }

    if (const char* configured = std::getenv("XDG_RUNTIME_DIR"); configured && *configured)
        impl_->runtime_dir = configured;
    else
        impl_->runtime_dir = "/run/user/" + std::to_string(static_cast<unsigned long>(::getuid()));

    std::error_code fs_error;
    if (!std::filesystem::is_directory(impl_->runtime_dir, fs_error) || fs_error) {
        impl_->error = "XDG_RUNTIME_DIR is unavailable; enable the user's systemd session/linger";
        return false;
    }

    std::filesystem::remove(impl_->socket_path(), fs_error);
    if (!impl_->save_and_install_environment()) return false;

    // Ensure the user's normal PipeWire services are available before KWin
    // publishes the screencast node. Socket activation makes this a no-op on
    // already-running Plasma sessions.
    (void)std::system("systemctl --user start pipewire.socket pipewire.service wireplumber.service >/dev/null 2>&1");

    const int width = std::clamp(requested.width, 640, 7680);
    const int height = std::clamp(requested.height, 480, 4320);

    std::ostringstream body;
    body << "export XDG_RUNTIME_DIR=" << shell_quote(impl_->runtime_dir) << "; "
         << "export WAYLAND_DISPLAY=" << shell_quote(impl_->display_name) << "; "
         << "export XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=KDE XDG_SESSION_DESKTOP=KDE KDE_FULL_SESSION=true QT_QPA_PLATFORM=wayland; "
         << "export KWIN_WAYLAND_NO_PERMISSION_CHECKS=1; "
         << "kwin_wayland --virtual --xwayland --no-lockscreen --width " << width
         << " --height " << height << " --socket " << shell_quote(impl_->display_name)
         << " >/dev/null 2>&1 & kwin=$!; "
         << "i=0; while [ $i -lt 200 ] && [ ! -S " << shell_quote(impl_->socket_path().string())
         << " ]; do i=$((i+1)); sleep 0.025; done; "
         << "[ -S " << shell_quote(impl_->socket_path().string()) << " ] || { kill $kwin 2>/dev/null; exit 70; }; "
         << "command -v kactivitymanagerd >/dev/null 2>&1 && kactivitymanagerd >/dev/null 2>&1 & "
         << "command -v kded6 >/dev/null 2>&1 && kded6 >/dev/null 2>&1 & "
         << "plasmashell >/dev/null 2>&1 & "
         << "command -v krunner >/dev/null 2>&1 && krunner >/dev/null 2>&1 & "
         << "wait $kwin";

    const bool have_user_bus = std::filesystem::exists(std::filesystem::path(impl_->runtime_dir) / "bus");
    std::string command = body.str();
    if (!have_user_bus) {
        if (!executable_on_path("dbus-run-session")) {
            impl_->error = "managed headless Plasma requires a user D-Bus session or dbus-run-session";
            impl_->restore_environment();
            return false;
        }
        command = "exec dbus-run-session -- /bin/sh -lc " + shell_quote(command);
    } else {
        command = "exec /bin/sh -lc " + shell_quote(command);
    }

    const pid_t child = ::fork();
    if (child < 0) {
        impl_->error = "could not fork the managed Plasma session";
        impl_->restore_environment();
        return false;
    }
    if (child == 0) {
        (void)::setsid();
        ::execl("/bin/sh", "sh", "-lc", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    impl_->leader = child;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        if (::waitpid(impl_->leader, &status, WNOHANG) == impl_->leader) {
            impl_->leader = -1;
            impl_->error = "managed KWin session exited before its Wayland socket became ready";
            impl_->restore_environment();
            return false;
        }
        if (std::filesystem::exists(impl_->socket_path())) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    impl_->error = "managed KWin session did not create its Wayland socket";
    stop();
    return false;
}

bool HeadlessSession::running() const
{
    if (!impl_ || impl_->leader <= 0) return false;
    return ::kill(impl_->leader, 0) == 0 && std::filesystem::exists(impl_->socket_path());
}

void HeadlessSession::stop()
{
    if (!impl_) return;
    if (impl_->leader > 0) {
        (void)::kill(-impl_->leader, SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        int status = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            if (::waitpid(impl_->leader, &status, WNOHANG) == impl_->leader) {
                impl_->leader = -1;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (impl_->leader > 0) {
            (void)::kill(-impl_->leader, SIGKILL);
            (void)::waitpid(impl_->leader, &status, 0);
            impl_->leader = -1;
        }
    }
    if (!impl_->runtime_dir.empty()) {
        std::error_code error;
        std::filesystem::remove(impl_->socket_path(), error);
    }
    impl_->restore_environment();
}

std::string HeadlessSession::wayland_display() const
{
    return impl_ ? impl_->display_name : std::string{};
}

std::string HeadlessSession::last_error() const
{
    return impl_ ? impl_->error : "headless session unavailable";
}

}
