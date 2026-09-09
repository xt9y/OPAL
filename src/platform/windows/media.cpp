#include <opal/media.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
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

bool launch_process(const std::string& command, HANDLE stdin_handle, HANDLE stdout_handle,
                    HANDLE stderr_handle, HANDLE& process_handle)
{
    process_handle = nullptr;
    std::wstring command_line = shell_command(command);
    if (command_line.empty()) return false;
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_handle;
    startup.hStdOutput = stdout_handle;
    startup.hStdError = stderr_handle;

    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (!created) return false;
    CloseHandle(process.hThread);
    process_handle = process.hProcess;
    return true;
}

struct SpawnResult {
    HANDLE process = nullptr;
    HANDLE parent_pipe = nullptr;
    HANDLE wait_event = nullptr;
};

SpawnResult spawn_capture_with_pipe(const std::string& command)
{
    if (command.empty()) return {};

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) return {};
    if (!SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        return {};
    }

    HANDLE process = nullptr;
    if (!launch_process(command, GetStdHandle(STD_INPUT_HANDLE), write_pipe, write_pipe, process)) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        return {};
    }
    CloseHandle(write_pipe);
    return {process, read_pipe, nullptr};
}

std::wstring unique_sink_pipe_name()
{
    static std::atomic<unsigned long long> serial{1};
    return L"\\\\.\\pipe\\opal-input-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::to_wstring(serial.fetch_add(1, std::memory_order_relaxed));
}

bool connect_local_named_pipe(HANDLE server)
{
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return false;
    OVERLAPPED overlapped{};
    overlapped.hEvent = event;

    bool connected = false;
    if (ConnectNamedPipe(server, &overlapped)) {
        connected = true;
    } else {
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED) {
            connected = true;
        } else if (error == ERROR_IO_PENDING) {
            connected = WaitForSingleObject(event, 1000) == WAIT_OBJECT_0;
            if (!connected) {
                (void)CancelIoEx(server, &overlapped);
                (void)WaitForSingleObject(event, INFINITE);
            }
        }
    }
    CloseHandle(event);
    return connected;
}

SpawnResult spawn_sink_with_pipe(const std::string& command)
{
    if (command.empty()) return {};
    const std::wstring pipe_name = unique_sink_pipe_name();

    HANDLE parent_write = CreateNamedPipeW(
        pipe_name.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 0, 0, nullptr);
    if (parent_write == INVALID_HANDLE_VALUE) return {};

    SECURITY_ATTRIBUTES child_security{};
    child_security.nLength = sizeof(child_security);
    child_security.bInheritHandle = TRUE;
    HANDLE child_read = CreateFileW(pipe_name.c_str(), GENERIC_READ, 0, &child_security,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (child_read == INVALID_HANDLE_VALUE) {
        CloseHandle(parent_write);
        return {};
    }
    if (!connect_local_named_pipe(parent_write)) {
        CloseHandle(child_read);
        CloseHandle(parent_write);
        return {};
    }

    HANDLE write_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!write_event) {
        CloseHandle(child_read);
        CloseHandle(parent_write);
        return {};
    }

    HANDLE process = nullptr;
    if (!launch_process(command, child_read, GetStdHandle(STD_OUTPUT_HANDLE),
                        GetStdHandle(STD_ERROR_HANDLE), process)) {
        CloseHandle(write_event);
        CloseHandle(child_read);
        CloseHandle(parent_write);
        return {};
    }
    CloseHandle(child_read);
    return {process, parent_write, write_event};
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

DWORD remaining_wait_ms(ULONGLONG deadline, int timeout_ms)
{
    if (timeout_ms < 0) return INFINITE;
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline) return 0;
    return static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, MAXDWORD - 1ULL));
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
    const auto spawned = spawn_capture_with_pipe(command);
    if (!spawned.process || !spawned.parent_pipe) return {};
    return {store_process(spawned.process), store_io(spawned.parent_pipe)};
}

int read_capture(CaptureProcess& capture, void* buffer, std::size_t size, int timeout_ms)
{
    HANDLE process = native_process(capture.process);
    HANDLE pipe = native_io(capture.io);
    if (!process || !pipe || !buffer || size == 0 || size > static_cast<std::size_t>(MAXDWORD)) return -1;

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
    const auto spawned = spawn_sink_with_pipe(command);
    if (!spawned.process || !spawned.parent_pipe || !spawned.wait_event) return {};
    const bool compact = command.find("opal-input") != std::string::npos;
    return {store_process(spawned.process), store_io(spawned.parent_pipe), compact,
            store_io(spawned.wait_event)};
}

bool write_sink_timeout(SinkProcess& sink, const void* data, std::size_t size, int timeout_ms)
{
    HANDLE process = native_process(sink.process);
    HANDLE pipe = native_io(sink.io);
    HANDLE event = native_io(sink.wait_io);
    if (!process || !pipe || !event || (!data && size != 0) ||
        size > static_cast<std::size_t>(MAXDWORD)) return false;
    if (!process_running(process)) return false;
    if (size == 0) return true;

    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t offset = 0;
    const ULONGLONG deadline = timeout_ms < 0 ? 0 :
        GetTickCount64() + static_cast<ULONGLONG>(std::max(0, timeout_ms));

    while (offset < size) {
        if (!process_running(process)) return false;
        ResetEvent(event);
        OVERLAPPED overlapped{};
        overlapped.hEvent = event;
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(size - offset, 4096));

        const BOOL completed = WriteFile(pipe, bytes + offset, request, nullptr, &overlapped);
        if (!completed) {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING) return false;
            const DWORD wait = WaitForSingleObject(event, remaining_wait_ms(deadline, timeout_ms));
            if (wait != WAIT_OBJECT_0) {
                (void)CancelIoEx(pipe, &overlapped);
                (void)WaitForSingleObject(event, INFINITE);
                return false;
            }
        }

        DWORD written = 0;
        if (!GetOverlappedResult(pipe, &overlapped, &written, FALSE) || written == 0) return false;
        offset += written;
        if (offset < size && timeout_ms >= 0 && GetTickCount64() >= deadline) return false;
    }
    return true;
}

void stop_sink(SinkProcess& sink)
{
    HANDLE event = native_io(sink.wait_io);
    stop_process(native_process(sink.process), native_io(sink.io));
    if (event) CloseHandle(event);
    sink = {};
}

}
