#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

#include <opal/client.hpp>
#include <opal/host.hpp>
#include <opal/realtime.hpp>
#include <opal/setup.hpp>
#include <opal/system.hpp>
#include <opal/wake.hpp>

#include <csignal>
#include <iostream>
#include <string>

#if defined(_WIN32)
namespace {

constexpr wchar_t kOpalHostMutexName[] = L"Local\\xt9y.OPAL.HostDaemon";
constexpr wchar_t kOpalHostStopEventName[] = L"Local\\xt9y.OPAL.HostStop";

bool windows_host_mutex_running()
{
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kOpalHostMutexName);
    if (!mutex) return false;
    const DWORD wait = WaitForSingleObject(mutex, 0);
    const bool running = wait == WAIT_TIMEOUT;
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) (void)ReleaseMutex(mutex);
    CloseHandle(mutex);
    return running;
}

bool windows_legacy_host_running()
{
    if (!windows_host_mutex_running()) return false;
    HANDLE stop_event = OpenEventW(SYNCHRONIZE, FALSE, kOpalHostStopEventName);
    if (stop_event) {
        CloseHandle(stop_event);
        return false;
    }
    return true;
}

bool windows_opal_process_name(const wchar_t* name)
{
    return CompareStringOrdinal(name, -1, L"opal.exe", -1, TRUE) == CSTR_EQUAL ||
           CompareStringOrdinal(name, -1, L"opal", -1, TRUE) == CSTR_EQUAL;
}

bool windows_wait_for_host_exit(DWORD timeout_ms)
{
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    do {
        if (!windows_host_mutex_running()) return true;
        Sleep(10);
    } while (GetTickCount64() < deadline);
    return !windows_host_mutex_running();
}

bool windows_take_over_legacy_host()
{
    if (!windows_legacy_host_running()) return true;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        std::cerr << "OPAL legacy host process snapshot failed error=" << GetLastError() << '\n';
        return false;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD candidate = 0;
    unsigned matches = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == GetCurrentProcessId()) continue;
            if (!windows_opal_process_name(entry.szExeFile)) continue;
            candidate = entry.th32ProcessID;
            ++matches;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    if (matches != 1 || candidate == 0) {
        std::cerr << "OPAL legacy host takeover found " << matches
                  << " other opal processes; refusing to terminate an ambiguous process.\n";
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                 FALSE, candidate);
    if (!process) {
        std::cerr << "OPAL legacy host takeover could not open pid=" << candidate
                  << " error=" << GetLastError() << '\n';
        return false;
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process, &exit_code)) {
        const DWORD error = GetLastError();
        CloseHandle(process);
        std::cerr << "OPAL legacy host takeover could not inspect pid=" << candidate
                  << " error=" << error << '\n';
        return false;
    }
    if (exit_code != STILL_ACTIVE) {
        CloseHandle(process);
        return windows_wait_for_host_exit(1000);
    }

    const bool terminated = TerminateProcess(process, 0) != FALSE;
    const DWORD error = terminated ? ERROR_SUCCESS : GetLastError();
    if (terminated) (void)WaitForSingleObject(process, 3000);
    CloseHandle(process);
    if (!terminated) {
        std::cerr << "OPAL legacy host takeover could not terminate pid=" << candidate
                  << " error=" << error << '\n';
        return false;
    }
    if (!windows_wait_for_host_exit(3000)) {
        std::cerr << "OPAL legacy host process exited but its host mutex remained active.\n";
        return false;
    }

    std::cout << "OPAL migrated legacy Windows host daemon.\n";
    return true;
}

bool windows_prepare_host_lifecycle()
{
    return !windows_legacy_host_running() || windows_take_over_legacy_host();
}

}
#endif

static void help()
{
    std::cout << R"(OPAL - performance-first Linux + Apple Silicon macOS + Windows remote desktop

Commands:
  opal                                      Wake and connect at up to 1080p / local refresh
  opal [--mode max|1080p|1440p|4k] [--fps 15-240]
                                            Connect with temporary stream overrides
  opal select                               Select a saved host and show connection details
  opal list                                 Alias for opal select
  opal new                                  Run OPAL setup / add another host
  opal remove                               Remove a saved host
  opal stop                                 Stop OPAL host services
  opal restart                              Restart host service or reconnect client
  opal clean                                Remove OPAL state
  opal doctor                               Check local OPAL requirements
  opal version                              Show the OPAL version
  opal help                                 Show this help

Tailscale is required on both computers. OPAL establishes its own authenticated,
end-to-end encrypted direct session over the tailnet with no public fallback service.
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
#if defined(_WIN32)
        if (!windows_prepare_host_lifecycle()) return 1;
#endif
        const int result = opal::host_service(false);
        if (result == 0) std::cout << "OPAL host stopped.\n";
        return result;
    }
    if (action == "restart" && argc == 2) {
#if defined(_WIN32)
        if (!windows_prepare_host_lifecycle()) return 1;
#endif
        const int stopped = opal::host_service(false);
        if (stopped != 0) return stopped;
        return opal::interactive_run();
    }
    if (action == "clean" && argc == 2) {
#if defined(_WIN32)
        if (!windows_prepare_host_lifecycle()) return 1;
#endif
        return opal::clean();
    }
    if ((action == "select" || action == "list") && argc == 2) return opal::interactive_select();
    if (action == "new" && argc == 2) return opal::interactive_setup();
    if (action == "remove" && argc == 2) return opal::interactive_remove();
    if (action == "doctor" && argc == 2) return opal::doctor();
    std::cerr << "Unknown command. Run 'opal help'.\n";
    return 2;
}
