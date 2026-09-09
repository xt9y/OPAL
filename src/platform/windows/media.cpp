#include <opal/media.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace opal {
namespace {

HANDLE native_process(ProcessNativeHandle value)
{
    return value == kInvalidProcessHandle ? nullptr : reinterpret_cast<HANDLE>(value);
}

HANDLE native_io(ProcessIoHandle value)
{
    return value == kInvalidProcessIoHandle ? nullptr : reinterpret_cast<HANDLE>(value);
}

ProcessNativeHandle store_process(HANDLE value)
{
    return value ? reinterpret_cast<ProcessNativeHandle>(value) : kInvalidProcessHandle;
}

ProcessIoHandle store_io(HANDLE value)
{
    return value ? reinterpret_cast<ProcessIoHandle>(value) : kInvalidProcessIoHandle;
}

std::wstring utf8_to_wide(std::string_view text)
{
    if (text.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring out(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                            out.data(), required) != required) return {};
    return out;
}

std::wstring shell_command(const std::string& command)
{
    const auto wide = utf8_to_wide(command);
    if (wide.empty()) return {};
    return L"cmd.exe /d /s /c \"" + wide + L"\"";
}

struct SpawnResult {
    HANDLE process = nullptr;
    HANDLE parent_pipe = nullptr;
};

SpawnResult spawn_with_pipe(const std::string& command, bool capture_stdout)
{
    if (command.empty()) return {};

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) return {};

    HANDLE parent_pipe = capture_stdout ? read_pipe : write_pipe;
    HANDLE child_pipe = capture_stdout ? write_pipe : read_pipe;
    if (!SetHandleInformation(parent_pipe, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        return {};
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = capture_stdout ? GetStdHandle(STD_INPUT_HANDLE) : child_pipe;
    startup.hStdOutput = capture_stdout ? child_pipe : GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = capture_stdout ? child_pipe : GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION process{};
    std::wstring command_line = shell_command(command);
    if (command_line.empty()) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        return {};
    }
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    const BOOL created = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (!created) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        return {};
    }

    CloseHandle(process.hThread);
    CloseHandle(child_pipe);
    return {process.hProcess, parent_pipe};
}

bool process_running(HANDLE process)
{
    if (!process) return false;
    DWORD exit_code = 0;
    return GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
}

void stop_process(HANDLE process, HANDLE pipe)
{
    if (pipe) CloseHandle(pipe);
    if (!process) return;
    if (WaitForSingleObject(process, 500) == WAIT_TIMEOUT) {
        (void)TerminateProcess(process, 1);
        (void)WaitForSingleObject(process, 500);
    }
    CloseHandle(process);
}

}

std::string capture_command(bool, int, int, bool, const std::string&, int, int)
{
    // Windows production capture is DXGI + Media Foundation. The process
    // capture API remains only for platform-neutral helper plumbing.
    return {};
}

CaptureProcess start_capture(const std::string& command)
{
    const auto spawned = spawn_with_pipe(command, true);
    if (!spawned.process || !spawned.parent_pipe) return {};
    return {store_process(spawned.process), store_io(spawned.parent_pipe)};
}

int read_capture(CaptureProcess& capture, void* buffer, std::size_t size, int timeout_ms)
{
    HANDLE process = native_process(capture.process);
    HANDLE pipe = native_io(capture.io);
    if (!process || !pipe || !buffer || size == 0 || size > static_cast<std::size_t>(DWORD_MAX)) return -1;

    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(std::max(0, timeout_ms));
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
            const DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE) return 0;
            return -1;
        }
        if (available > 0) {
            DWORD read = 0;
            const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(size, available));
            if (!ReadFile(pipe, buffer, requested, &read, nullptr)) {
                return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
            }
            return static_cast<int>(read);
        }
        if (!process_running(process)) return 0;
        if (timeout_ms <= 0 || GetTickCount64() >= deadline) return -2;
        Sleep(1);
    }
}

void stop_capture(CaptureProcess& capture)
{
    stop_process(native_process(capture.process), native_io(capture.io));
    capture = {};
}

SinkProcess start_sink(const std::string& command)
{
    const auto spawned = spawn_with_pipe(command, false);
    if (!spawned.process || !spawned.parent_pipe) return {};
    const bool compact = command.find("opal-input") != std::string::npos;
    return {store_process(spawned.process), store_io(spawned.parent_pipe), compact};
}

bool write_sink_timeout(SinkProcess& sink, const void* data, std::size_t size, int timeout_ms)
{
    HANDLE process = native_process(sink.process);
    HANDLE pipe = native_io(sink.io);
    if (!process || !pipe || (!data && size != 0) || size > static_cast<std::size_t>(DWORD_MAX)) return false;
    if (!process_running(process)) return false;
    if (size == 0) return true;

    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t offset = 0;
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(std::max(0, timeout_ms));
    while (offset < size) {
        if (!process_running(process)) return false;
        DWORD written = 0;
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(size - offset, 4096));
        if (!WriteFile(pipe, bytes + offset, request, &written, nullptr) || written == 0) return false;
        offset += written;
        if (offset < size && timeout_ms >= 0 && GetTickCount64() >= deadline) return false;
    }
    return true;
}

void stop_sink(SinkProcess& sink)
{
    stop_process(native_process(sink.process), native_io(sink.io));
    sink = {};
}

}
