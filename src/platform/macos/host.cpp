#import <CoreGraphics/CoreGraphics.h>

#include <opal/media.hpp>

#include <fcntl.h>
#include <spawn.h>
#include <string>
#include <unistd.h>

extern char **environ;

namespace opal {

// The host may already have SDL, ScreenCaptureKit and VideoToolbox threads when
// the first remote input arrives. Avoid fork() in that multithreaded Cocoa
// process; posix_spawn creates the same stdin pipe/process-group contract that
// stop_sink()/write_sink_timeout() already expect.
SinkProcess macos_start_sink(const std::string& command)
{
    if (command.empty()) return {};
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0) return {};

    const auto close_pair = [&] {
        if (fds[0] >= 0) close(fds[0]);
        if (fds[1] >= 0) close(fds[1]);
        fds[0] = fds[1] = -1;
    };
    const auto set_cloexec = [](int fd) {
        const int flags = fcntl(fd, F_GETFD, 0);
        return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
    };
    if (!set_cloexec(fds[0]) || !set_cloexec(fds[1])) {
        close_pair();
        return {};
    }

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close_pair();
        return {};
    }
    if (posix_spawnattr_init(&attributes) != 0) {
        posix_spawn_file_actions_destroy(&actions);
        close_pair();
        return {};
    }

    bool configured = true;
    configured = configured && posix_spawn_file_actions_adddup2(&actions, fds[0], STDIN_FILENO) == 0;
    configured = configured && posix_spawn_file_actions_addclose(&actions, fds[0]) == 0;
    configured = configured && posix_spawn_file_actions_addclose(&actions, fds[1]) == 0;
    configured = configured && posix_spawnattr_setpgroup(&attributes, 0) == 0;
    configured = configured && posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP) == 0;

    pid_t pid = -1;
    int spawn_error = -1;
    if (configured) {
        char* const argv[] = {
            const_cast<char*>("sh"),
            const_cast<char*>("-c"),
            const_cast<char*>(command.c_str()),
            nullptr
        };
        spawn_error = posix_spawn(&pid, "/bin/sh", &actions, &attributes, argv, environ);
    }

    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (!configured || spawn_error != 0 || pid <= 0) {
        close_pair();
        return {};
    }

    close(fds[0]);
    fds[0] = -1;
    const int flags = fcntl(fds[1], F_GETFL, 0);
    if (flags < 0 || fcntl(fds[1], F_SETFL, flags | O_NONBLOCK) != 0) {
        close(fds[1]);
        fds[1] = -1;
        (void)kill(-pid, SIGTERM);
        return {};
    }

    const bool compact_input = command.find("opal-input") != std::string::npos;
    return {pid, fds[1], compact_input};
}

}

#define start_sink macos_start_sink
#define host_setup macos_host_setup_impl
#define host_run macos_host_run_impl
#define host_daemon macos_host_daemon_impl
#include "../../host.cpp"
#undef host_setup
#undef host_run
#undef host_daemon
#undef start_sink

namespace opal {
namespace {

void print_screen_permission_help()
{
    std::cerr << "OPAL macOS host requires Screen Recording permission. Enable it in System Settings > Privacy & Security > Screen & System Audio Recording.\n";
}

void print_accessibility_permission_help()
{
    std::cerr << "OPAL macOS host requires Accessibility permission for the opal-input helper. Enable opal-input in System Settings > Privacy & Security > Accessibility.\n";
}

bool input_helper_access(const char* mode, bool quiet)
{
    if (!mode || !*mode) return false;
    const std::string helper = input_helper_command();
    if (helper.empty()) return false;
    std::string command = helper + " " + mode;
    if (quiet) command += " >/dev/null 2>&1";
    return std::system(command.c_str()) == 0;
}

bool macos_host_permissions()
{
    bool ok = true;
    if (!CGPreflightScreenCaptureAccess()) {
        print_screen_permission_help();
        ok = false;
    }
    if (!input_helper_access("--check-access", true)) {
        print_accessibility_permission_help();
        ok = false;
    }
    return ok;
}

bool request_macos_host_permissions()
{
    bool screen_ok = CGPreflightScreenCaptureAccess();
    if (!screen_ok) screen_ok = CGRequestScreenCaptureAccess();

    const bool accessibility_ok = input_helper_access("--request-access", false);

    if (!screen_ok) print_screen_permission_help();
    if (!accessibility_ok) print_accessibility_permission_help();
    if (!screen_ok || !accessibility_ok) {
        std::cerr << "Grant the permissions above, then run 'opal' again to finish host setup.\n";
        return false;
    }
    return true;
}

}

int host_setup()
{
    if (macos_host_setup_impl() != 0) return 1;
    return request_macos_host_permissions() ? 0 : 1;
}

int host_run()
{
    if (!macos_host_permissions()) return 1;
    return macos_host_run_impl();
}

int host_daemon()
{
    if (!macos_host_permissions()) return 1;
    return macos_host_daemon_impl();
}

}
