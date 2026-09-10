#pragma once

#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif

namespace opal::idd {

inline constexpr std::uint32_t kProtocolVersion = 1;
inline constexpr std::uint32_t kFrameMagic = 0x4f50414c; // OPAL
inline constexpr std::uint32_t kMaxWidth = 3840;
inline constexpr std::uint32_t kMaxHeight = 2160;
inline constexpr std::uint32_t kBytesPerPixel = 4;
inline constexpr std::size_t kMaxFrameBytes =
    static_cast<std::size_t>(kMaxWidth) * static_cast<std::size_t>(kMaxHeight) * kBytesPerPixel;

inline constexpr wchar_t kDevicePath[] = L"\\\\.\\OpalDisplay";

#ifdef _WIN32
inline constexpr DWORD kIoctlGetStatus = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS);
inline constexpr DWORD kIoctlCreateMonitor = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS);
inline constexpr DWORD kIoctlSetMode = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS);
inline constexpr DWORD kIoctlDestroyMonitor = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS);
inline constexpr DWORD kIoctlGetFrame = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_OUT_DIRECT, FILE_READ_ACCESS);
#endif

struct DisplayModeRequest {
    std::uint32_t version = kProtocolVersion;
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    std::uint32_t refresh_hz = 60;
};

struct DriverStatus {
    std::uint32_t version = kProtocolVersion;
    std::uint32_t adapter_ready = 0;
    std::uint32_t monitor_active = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t refresh_hz = 0;
    std::uint32_t reserved = 0;
};

struct FrameRequest {
    std::uint32_t version = kProtocolVersion;
    std::uint32_t reserved = 0;
    std::int64_t last_sequence = 0;
};

struct alignas(64) SharedFrameHeader {
    std::uint32_t magic = kFrameMagic;
    std::uint32_t version = kProtocolVersion;
    std::int64_t sequence = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    std::uint32_t format = 0;
    std::uint64_t qpc = 0;
    std::uint32_t bytes = 0;
    std::uint32_t reserved = 0;
};

inline constexpr std::size_t kFrameReplyBytes = sizeof(SharedFrameHeader) + kMaxFrameBytes;

}
