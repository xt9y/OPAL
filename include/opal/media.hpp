#pragma once

#include <opal/media_profile.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace opal {

using ProcessNativeHandle = std::intptr_t;
#if defined(_WIN32)
using ProcessIoHandle = std::intptr_t;
#else
using ProcessIoHandle = int;
#endif
inline constexpr ProcessNativeHandle kInvalidProcessHandle = -1;
inline constexpr ProcessIoHandle kInvalidProcessIoHandle = -1;

struct CaptureProcess {
    union {
        ProcessNativeHandle process;
        ProcessNativeHandle pid; // transitional source alias; not a POSIX type
    };
    union {
        ProcessIoHandle io;
        ProcessIoHandle fd; // transitional source alias; not a POSIX type
    };

    constexpr CaptureProcess() noexcept : process(kInvalidProcessHandle), io(kInvalidProcessIoHandle) {}
    constexpr CaptureProcess(ProcessNativeHandle native_process, ProcessIoHandle native_io) noexcept
        : process(native_process), io(native_io) {}
    bool valid() const noexcept { return process != kInvalidProcessHandle && io != kInvalidProcessIoHandle; }
};

struct SinkProcess {
    union {
        ProcessNativeHandle process;
        ProcessNativeHandle pid; // transitional source alias; not a POSIX type
    };
    union {
        ProcessIoHandle io;
        ProcessIoHandle fd; // transitional source alias; not a POSIX type
    };
    ProcessIoHandle wait_io = kInvalidProcessIoHandle;
    bool compact_input = false;

    constexpr SinkProcess() noexcept : process(kInvalidProcessHandle), io(kInvalidProcessIoHandle) {}
    constexpr SinkProcess(ProcessNativeHandle native_process, ProcessIoHandle native_io, bool compact = false,
                          ProcessIoHandle native_wait_io = kInvalidProcessIoHandle) noexcept
        : process(native_process), io(native_io), wait_io(native_wait_io), compact_input(compact) {}
    bool valid() const noexcept { return process != kInvalidProcessHandle && io != kInvalidProcessIoHandle; }
};

std::string capture_command(bool gpu_screen_recorder, int fps, int bitrate_kbps, bool audio,
                            const std::string &portal_token_file = "", int max_width = 0,
                            int max_height = 0);
CaptureProcess start_capture(const std::string &command);
int read_capture(CaptureProcess &capture, void *buffer, std::size_t size, int timeout_ms);
void stop_capture(CaptureProcess &capture);
SinkProcess start_sink(const std::string &command);
bool write_sink_timeout(SinkProcess &sink, const void *data, std::size_t size, int timeout_ms);
void stop_sink(SinkProcess &sink);

}
