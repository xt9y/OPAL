#include <opal/client.hpp>
#include <opal/host.hpp>
#include <opal/realtime.hpp>
#include <opal/setup.hpp>
#include <opal/system.hpp>
#include <opal/wake.hpp>

#include <csignal>
#include <iostream>
#include <string>

static void help()
{
    std::cout << R"(OPAL - performance-first Linux + Apple Silicon macOS + Windows remote desktop

Commands:
  opal                                      Wake and connect at up to 1080p / local refresh
  opal [--mode max|1080p|1440p|4k] [--fps 15-240]
                                            Connect with temporary stream overrides
  opal select                               Select a saved host
  opal new                                  Run OPAL setup / add another host
  opal remove                               Remove a saved host
  opal stop                                 Stop OPAL host services
  opal restart                              Restart OPAL services
  opal clean                                Remove OPAL state
  opal doctor                               Check local OPAL requirements
  opal version                              Show the OPAL version
  opal help                                 Show this help

Networking is built into OPAL: signed rendezvous, direct end-to-end encrypted UDP,
and blind encrypted relay fallback when direct NAT traversal is unavailable.
Default FPS follows the client display refresh up to 240 Hz; --fps always overrides it.
Stream overrides apply only to the current connection. Resolution modes never upscale the host.
Config lives in the platform OPAL data directory (or OPAL_HOME for testing).
Release remote control with Ctrl+Alt+Shift+W; quit with Ctrl+Alt+Shift+Q.
)";
}

static bool parse_fps(const std::string& value, int& fps)
{
    try {
        size_t used = 0;
        const int parsed = std::stoi(value, &used);
        if (used != value.size() || parsed < 15 || parsed > 240) return false;
        fps = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

static int run_stream_flags(int argc, char** argv)
{
    opal::StreamOptions stream;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--mode") {
            if (i + 1 >= argc || !opal::stream_mode_limit(argv[++i], stream.max_width, stream.max_height)) {
                std::cerr << "invalid --mode; expected max, 1080p, 1440p, or 4k\n";
                return 2;
            }
        } else if (flag == "--fps") {
            if (i + 1 >= argc || !parse_fps(argv[++i], stream.fps)) {
                std::cerr << "invalid --fps; expected an integer from 15 to 240\n";
                return 2;
            }
            stream.automatic_fps = false;
        } else {
            std::cerr << "Unknown option. Run 'opal help'.\n";
            return 2;
        }
    }
    return opal::interactive_run(stream);
}

int main(int argc, char** argv)
{
#if !defined(_WIN32)
    signal(SIGPIPE, SIG_IGN);
#endif
    (void)opal::set_low_latency_timer_slack();
    if (argc == 1) return opal::interactive_run();

    const std::string action = argv[1];
    if (action == "--internal-host-daemon" && argc == 2) return opal::host_daemon();
    if (action == "--internal-bridge-run" && argc == 2) return opal::run_bridge(47992);
    if (action == "--internal-host-setup" && argc == 2) return opal::host_setup();
    if (action == "--internal-host-run" && argc == 2) return opal::host_run();
    if (action == "--internal-connect" && argc >= 3 && argc <= 4)
        return opal::client_connect(argv[2], argc == 4 ? argv[3] : "");
    if (action == "--mode" || action == "--fps") return run_stream_flags(argc, argv);
    if (action == "help" || action == "--help" || action == "-h") { help(); return 0; }
    if (action == "version" || action == "--version") { std::cout << "OPAL 0.2.0\n"; return 0; }
    if (action == "stop" && argc == 2) {
        const int result = opal::host_service(false);
        if (result == 0) std::cout << "OPAL host stopped.\n";
        return result;
    }
    if (action == "restart" && argc == 2) return opal::restart_services();
    if (action == "clean" && argc == 2) return opal::clean();
    if (action == "select" && argc == 2) return opal::interactive_select();
    if (action == "new" && argc == 2) return opal::interactive_setup();
    if (action == "remove" && argc == 2) return opal::interactive_remove();
    if (action == "doctor" && argc == 2) return opal::doctor();
    std::cerr << "Unknown command. Run 'opal help'.\n";
    return 2;
}
