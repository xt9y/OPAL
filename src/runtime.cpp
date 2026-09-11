#include <opal/runtime.hpp>

#include <chrono>
#include <cerrno>
#include <csignal>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <libproc.h>
#include <mach-o/dyld.h>
#endif

namespace opal {
namespace {

struct ClientRecord {
    unsigned long long pid = 0;
    std::string executable;
};

std::filesystem::path record_path(const std::filesystem::path& root)
{
    return root / "client.pid";
}

#if defined(_WIN32)
std::string utf8_from_wide(const wchar_t* value, int length)
{
    if (!value || length <= 0) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value, length, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string out(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value, length, out.data(), bytes, nullptr, nullptr) != bytes)
        return {};
    return out;
}

std::wstring wide_from_utf8(const std::string& value)
{
    if (value.empty()) return {};
    const int chars = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (chars <= 0) return {};
    std::wstring out(static_cast<std::size_t>(chars), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), chars) != chars)
        return {};
    return out;
}

std::string current_executable_identity()
{
    std::vector<wchar_t> buffer(1024);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!length) return {};
        if (length < buffer.size() - 1) return utf8_from_wide(buffer.data(), static_cast<int>(length));
        if (buffer.size() >= 32768) return {};
        buffer.resize(buffer.size() * 2);
    }
}

std::string process_executable_identity(unsigned long long pid)
{
    if (!pid || pid > 0xffffffffULL) return {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!process) return {};
    std::vector<wchar_t> buffer(32768);
    DWORD length = static_cast<DWORD>(buffer.size());
    const bool ok = QueryFullProcessImageNameW(process, 0, buffer.data(), &length) != FALSE && length > 0;
    CloseHandle(process);
    return ok ? utf8_from_wide(buffer.data(), static_cast<int>(length)) : std::string{};
}

bool same_executable(const std::string& actual, const std::string& expected)
{
    const auto actual_wide = wide_from_utf8(actual);
    const auto expected_wide = wide_from_utf8(expected);
    return !actual_wide.empty() && !expected_wide.empty() &&
           CompareStringOrdinal(actual_wide.c_str(), -1, expected_wide.c_str(), -1, TRUE) == CSTR_EQUAL;
}

unsigned long long current_pid()
{
    return static_cast<unsigned long long>(GetCurrentProcessId());
}
#else
std::string process_executable_identity(unsigned long long pid)
{
#if defined(__linux__)
    std::string link = "/proc/" + std::to_string(pid) + "/exe";
    std::string buffer(4096, '\0');
    const auto size = readlink(link.c_str(), buffer.data(), buffer.size() - 1);
    if (size <= 0) return {};
    buffer.resize(static_cast<std::size_t>(size));
    constexpr const char deleted[] = " (deleted)";
    if (buffer.size() > sizeof(deleted) - 1 && buffer.ends_with(deleted))
        buffer.resize(buffer.size() - (sizeof(deleted) - 1));
    return buffer;
#elif defined(__APPLE__)
    if (!pid || pid > static_cast<unsigned long long>(std::numeric_limits<int>::max())) return {};
    char buffer[PROC_PIDPATHINFO_MAXSIZE]{};
    if (proc_pidpath(static_cast<int>(pid), buffer, sizeof(buffer)) <= 0) return {};
    return buffer;
#else
    (void)pid;
    return {};
#endif
}

std::string current_executable_identity()
{
#if defined(__APPLE__)
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    if (!size) return {};
    std::string buffer(static_cast<std::size_t>(size), '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    buffer.resize(std::char_traits<char>::length(buffer.c_str()));
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(buffer, error);
    return error ? buffer : canonical.string();
#else
    return process_executable_identity(static_cast<unsigned long long>(getpid()));
#endif
}

bool same_executable(const std::string& actual, const std::string& expected)
{
    if (actual.empty() || expected.empty()) return false;
    std::error_code actual_error, expected_error;
    const auto actual_normal = std::filesystem::weakly_canonical(actual, actual_error);
    const auto expected_normal = std::filesystem::weakly_canonical(expected, expected_error);
    return (actual_error ? std::filesystem::path(actual).lexically_normal() : actual_normal) ==
           (expected_error ? std::filesystem::path(expected).lexically_normal() : expected_normal);
}

unsigned long long current_pid()
{
    return static_cast<unsigned long long>(getpid());
}
#endif

bool read_record(const std::filesystem::path& root, ClientRecord& record)
{
    std::ifstream input(record_path(root));
    if (!(input >> record.pid) || record.pid == 0) return false;
    input.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    return static_cast<bool>(std::getline(input, record.executable)) && !record.executable.empty();
}

void remove_record(const std::filesystem::path& root)
{
    std::error_code error;
    std::filesystem::remove(record_path(root), error);
}

bool process_matches(const ClientRecord& record)
{
    if (!record.pid || record.pid == current_pid()) return false;
    return same_executable(process_executable_identity(record.pid), record.executable);
}

#if !defined(_WIN32)
bool process_alive(unsigned long long pid)
{
    if (!pid || pid > static_cast<unsigned long long>(std::numeric_limits<pid_t>::max())) return false;
#if defined(__linux__)
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (std::getline(stat, line)) {
        const auto close = line.rfind(')');
        if (close != std::string::npos && close + 2 < line.size() && line[close + 2] == 'Z') return false;
    }
#endif
    if (kill(static_cast<pid_t>(pid), 0) == 0) return true;
    return errno == EPERM;
}

bool wait_for_exit(unsigned long long pid, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!process_alive(pid)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return !process_alive(pid);
}
#endif

}

ClientRuntimeLease::ClientRuntimeLease(std::filesystem::path root) : root_(std::move(root)) {}

ClientRuntimeLease::~ClientRuntimeLease()
{
    if (!active_) return;
    ClientRecord record;
    if (read_record(root_, record) && record.pid == pid_) remove_record(root_);
}

bool ClientRuntimeLease::acquire()
{
    if (active_) return true;
    std::error_code error;
    std::filesystem::create_directories(root_, error);
    if (error) return false;

    ClientRecord existing;
    if (read_record(root_, existing)) {
        if (process_matches(existing)) return false;
        remove_record(root_);
    }

    const auto executable = current_executable_identity();
    if (executable.empty()) return false;
    pid_ = current_pid();
    std::ofstream output(record_path(root_), std::ios::out | std::ios::trunc);
    if (!output) return false;
    output << pid_ << '\n' << executable << '\n';
    if (!output.good()) {
        output.close();
        remove_record(root_);
        return false;
    }
    active_ = true;
    return true;
}

bool client_runtime_running(const std::filesystem::path& root)
{
    ClientRecord record;
    if (!read_record(root, record)) {
        remove_record(root);
        return false;
    }
    if (!process_matches(record)) {
        remove_record(root);
        return false;
    }
    return true;
}

bool stop_client_runtime(const std::filesystem::path& root)
{
    ClientRecord record;
    if (!read_record(root, record)) {
        remove_record(root);
        return true;
    }
    if (!process_matches(record)) {
        remove_record(root);
        return true;
    }

#if defined(_WIN32)
    if (record.pid > 0xffffffffULL) return false;
    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                 FALSE, static_cast<DWORD>(record.pid));
    if (!process) return false;
    std::vector<wchar_t> buffer(32768);
    DWORD length = static_cast<DWORD>(buffer.size());
    const bool identity_ok = QueryFullProcessImageNameW(process, 0, buffer.data(), &length) != FALSE &&
                             length > 0 && same_executable(
                                 utf8_from_wide(buffer.data(), static_cast<int>(length)), record.executable);
    if (!identity_ok) {
        CloseHandle(process);
        remove_record(root);
        return true;
    }
    const bool terminated = TerminateProcess(process, 0) != FALSE;
    const bool exited = terminated && WaitForSingleObject(process, 3000) == WAIT_OBJECT_0;
    CloseHandle(process);
    if (!exited) return false;
#else
    if (record.pid > static_cast<unsigned long long>(std::numeric_limits<pid_t>::max())) return false;
    const auto pid = static_cast<pid_t>(record.pid);
    if (kill(pid, SIGTERM) != 0 && errno != ESRCH) return false;
    if (!wait_for_exit(record.pid, std::chrono::milliseconds(1000))) {
        if (kill(pid, SIGKILL) != 0 && errno != ESRCH) return false;
        if (!wait_for_exit(record.pid, std::chrono::milliseconds(1000))) return false;
    }
#endif

    remove_record(root);
    return true;
}

}
