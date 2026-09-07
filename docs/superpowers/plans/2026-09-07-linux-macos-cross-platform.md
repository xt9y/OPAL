# Linux + macOS Cross-Platform Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make OPAL natively support Linux and Apple Silicon macOS as both client and host while preserving the existing wire protocol and Linux low-latency fast paths, with public interfaces suitable for a later Windows backend.

**Architecture:** Keep pairing, session, crypto, discovery, relay, packetization, feedback and media-control logic platform-neutral. Extract OS-owned operations behind narrow C++ interfaces selected by the build. Linux retains PipeWire/uinput/Linux UDP batching; macOS adds SDL3 client support, ScreenCaptureKit, VideoToolbox, CGEvent, NSPasteboard and native system-audio capture. Each extraction lands and passes Linux regression gates before its macOS implementation is connected.

**Tech Stack:** C++20, SDL3, OpenSSL, FFmpeg/libavcodec/libavutil/libswresample, Linux PipeWire/libportal/uinput/sendmmsg/recvmmsg, macOS ScreenCaptureKit/CoreMedia/CoreVideo/VideoToolbox/CoreGraphics/AppKit/CoreAudio, Objective-C++, Make.

**Spec:** `docs/superpowers/specs/2026-09-07-linux-macos-cross-platform-design.md`

## Global Constraints

- Implement Linux + Apple Silicon macOS now; do not add Windows source files, targets, dependencies or CI jobs.
- New public interfaces must not require POSIX file descriptors, sockaddr types, Linux evdev types, Objective-C objects or Apple framework types.
- Preserve connection-code format, authentication/encryption, media packet framing, reliable control, video feedback/IDR recovery, clipboard messages, input command semantics and direct/relay behavior.
- SDL3 remains the baseline client window/event/input/audio-output layer.
- Preserve Linux PipeWire + libportal capture, DMA-BUF opportunities, `/dev/uinput`, current hardware encoder selection and Linux UDP batching.
- macOS production host video uses ScreenCaptureKit + persistent VideoToolbox; no shell-out FFmpeg production capture/encode path.
- Apple framework types stay in `.mm` implementation files/private source headers.
- Keep latest-first bounded queues; portability must not introduce deep capture/encode/decode/presentation buffering.
- `make test`, `make test-hpi` and existing Linux latency behavior are regression boundaries.
- Every behavior change is test-first and every task ends with a reviewable commit.

---

### Task 1: Add common platform/error contracts and platform-neutral wire keys

**Files:**
- Create: `include/opal/platform.hpp`
- Create: `include/opal/platform_error.hpp`
- Create: `include/opal/input_wire.hpp`
- Create: `tests/test_platform_contract.cpp`
- Modify: `include/opal/input.hpp`
- Modify: `src/input.cpp`
- Modify: `src/client.cpp`
- Modify: `tests/test_input.cpp`
- Modify: `Makefile.core`

**Interfaces:**

```cpp
enum class PlatformKind : std::uint8_t { Linux, MacOS, Unsupported };
PlatformKind current_platform() noexcept;

enum class PlatformComponent : std::uint8_t {
    Capture, Encoder, Decoder, Input, Clipboard, AudioCapture, Datagram, Presenter, System
};
enum class PlatformFailure : std::uint8_t {
    None, Unsupported, PermissionDenied, Unavailable, InvalidState, DependencyMissing, OsError
};
struct PlatformError {
    PlatformComponent component=PlatformComponent::System;
    PlatformFailure failure=PlatformFailure::None;
    std::string message;
    bool fallback_possible=false;
    explicit operator bool() const noexcept { return failure!=PlatformFailure::None; }
};

using WireKeyCode=std::uint16_t;
WireKeyCode wire_keycode_from_sdl_scancode(int scancode);
```

- [ ] **Step 1: Write the failing public-header contract test**

`tests/test_platform_contract.cpp` must assert stable key values and scan public headers for forbidden native includes/types:

```cpp
#include <opal/input_wire.hpp>
#include <fstream>
#include <sstream>
#include <cassert>
static std::string read_all(const char*p){std::ifstream in(p);std::ostringstream out;out<<in.rdbuf();return out.str();}
int main(){
    static_assert(opal::wire_key::Q==16);
    static_assert(opal::wire_key::W==17);
    static_assert(opal::wire_key::LeftCtrl==29);
    static_assert(opal::wire_key::A==30);
    static_assert(opal::wire_key::LeftShift==42);
    static_assert(opal::wire_key::LeftAlt==56);
    for(const char*p:{"include/opal/input.hpp","include/opal/input_wire.hpp","include/opal/platform.hpp","include/opal/platform_error.hpp"}){
        const auto s=read_all(p);
        assert(s.find("<linux/")==std::string::npos);
        assert(s.find("sockaddr_")==std::string::npos);
        assert(s.find("CGEventRef")==std::string::npos);
        assert(s.find("CVPixelBufferRef")==std::string::npos);
    }
}
```

- [ ] **Step 2: Run RED**

Run: `make test-platform-contract`

Expected: target/headers missing.

- [ ] **Step 3: Implement platform/error headers**

Implement the signatures above. `current_platform()` returns Linux for `__linux__`, MacOS for `__APPLE__`, otherwise Unsupported. Add string-name helpers for diagnostics.

- [ ] **Step 4: Create OPAL-owned wire-key values**

Define every key used by the current SDL mapping in `input_wire.hpp`, preserving current evdev numeric values. Required values are:

```cpp
namespace opal::wire_key {
inline constexpr WireKeyCode None=0, Esc=1, Num1=2, Num2=3, Num3=4, Num4=5, Num5=6, Num6=7, Num7=8, Num8=9, Num9=10, Num0=11;
inline constexpr WireKeyCode Minus=12, Equal=13, Backspace=14, Tab=15, Q=16, W=17, E=18, R=19, T=20, Y=21, U=22, I=23, O=24, P=25;
inline constexpr WireKeyCode LeftBrace=26, RightBrace=27, Enter=28, LeftCtrl=29, A=30, S=31, D=32, F=33, G=34, H=35, J=36, K=37, L=38;
inline constexpr WireKeyCode Semicolon=39, Apostrophe=40, Grave=41, LeftShift=42, Backslash=43, Z=44, X=45, C=46, V=47, B=48, N=49, M=50, Comma=51, Dot=52, Slash=53;
inline constexpr WireKeyCode RightShift=54, KpAsterisk=55, LeftAlt=56, Space=57, CapsLock=58, F1=59, F2=60, F3=61, F4=62, F5=63, F6=64, F7=65, F8=66, F9=67, F10=68;
inline constexpr WireKeyCode NumLock=69, ScrollLock=70, Kp7=71, Kp8=72, Kp9=73, KpMinus=74, Kp4=75, Kp5=76, Kp6=77, KpPlus=78, Kp1=79, Kp2=80, Kp3=81, Kp0=82, KpDot=83;
inline constexpr WireKeyCode F11=87, F12=88, KpEnter=96, RightCtrl=97, KpSlash=98, SysRq=99, RightAlt=100, Home=102, Up=103, PageUp=104, Left=105, Right=106, End=107, Down=108, PageDown=109, Insert=110, Delete=111, Pause=119, LeftMeta=125, RightMeta=126, Compose=127;
}
```

- [ ] **Step 5: Replace Linux-header mapping/chord code**

Remove `<linux/input-event-codes.h>` from `src/input.cpp` and `tests/test_input.cpp`. Rename `linux_keycode_from_sdl_scancode()` to `wire_keycode_from_sdl_scancode()` and return `wire_key::*`. Use `wire_key::*` for Ctrl+Alt+Shift+W/Q chord detection. Keep command strings and numeric wire behavior unchanged.

- [ ] **Step 6: Add target and verify GREEN**

Add `test-platform-contract` to `Makefile.core`; run `make test-platform-contract test-input`.

- [ ] **Step 7: Commit**

```bash
git add include/opal/platform.hpp include/opal/platform_error.hpp include/opal/input_wire.hpp include/opal/input.hpp src/input.cpp src/client.cpp tests/test_platform_contract.cpp tests/test_input.cpp Makefile.core
git commit -m "refactor: add platform-neutral protocol contracts"
```

### Task 2: Remove native socket types from the public UDP API and preserve Linux batching

**Files:**
- Create: `src/platform/posix/udp_native.hpp`
- Create: `src/platform/posix/udp_socket.cpp`
- Create: `src/platform/linux/udp_batch.cpp`
- Modify: `include/opal/udp_transport.hpp`
- Modify: `src/udp_transport.cpp`
- Modify: `tests/test_udp_transport.cpp`
- Modify: `Makefile.core`

**Interfaces:**

```cpp
using UdpNativeHandle=std::uintptr_t;
inline constexpr UdpNativeHandle kInvalidUdpHandle=~UdpNativeHandle{0};
struct UdpEndpoint { std::array<std::byte,128> native{}; std::uint32_t native_size=0; };
struct UdpSocket {
    UdpNativeHandle handle=kInvalidUdpHandle;
    std::uint16_t local_port=0;
    bool valid() const noexcept { return handle!=kInvalidUdpHandle; }
};
struct UdpReceiveSlot { std::span<std::uint8_t> buffer{}; UdpEndpoint source{}; std::size_t size=0; std::uint32_t kernel_drops=0; };
```

- [ ] **Step 1: Convert `test_udp_transport` to the new API before implementation**

Use `UdpSocket`/`UdpEndpoint` in all OPAL calls. Keep Linux socket-buffer/TCLASS assertions inside `#if defined(__linux__)` by converting `handle` to `int` only in the test.

- [ ] **Step 2: Run RED**

Run: `make test-udp-transport`.

Expected: new endpoint/socket API missing.

- [ ] **Step 3: Implement standard-only public transport types**

Remove `<sys/socket.h>` from `udp_transport.hpp`. Change resolve/send/receive APIs to consume `UdpSocket`/`UdpEndpoint` rather than `int`, `sockaddr_storage`, `socklen_t`.

- [ ] **Step 4: Put POSIX conversion details under `src/`**

`src/platform/posix/udp_native.hpp` may include POSIX socket headers and defines:

```cpp
int posix_fd(const UdpSocket&) noexcept;
bool endpoint_from_sockaddr(const sockaddr*,socklen_t,UdpEndpoint&);
bool endpoint_to_sockaddr(const UdpEndpoint&,sockaddr_storage&,socklen_t&);
```

Move ordinary socket creation, nonblocking configuration, DSCP/TCLASS, endpoint resolution, interface enumeration and single send/receive operations to `src/platform/posix/udp_socket.cpp`.

- [ ] **Step 5: Move Linux `sendmmsg`/`recvmmsg` unchanged behind private symbols**

`src/platform/linux/udp_batch.cpp` implements the batch functions used by `udp_transport.cpp`, retaining batch max 32, partial progress, kernel drop accounting and transient backpressure behavior.

- [ ] **Step 6: Verify Linux regression**

Run: `make test-udp-transport test-peer-session test-peer-session-relay test-direct-video-stress`.

- [ ] **Step 7: Commit**

```bash
git add include/opal/udp_transport.hpp src/udp_transport.cpp src/platform/posix/udp_native.hpp src/platform/posix/udp_socket.cpp src/platform/linux/udp_batch.cpp tests/test_udp_transport.cpp Makefile.core
git commit -m "refactor: isolate native UDP transport details"
```

### Task 3: Add portable/macOS UDP batch fallback

**Files:**
- Create: `src/platform/posix/udp_batch_fallback.hpp`
- Create: `src/platform/posix/udp_batch_fallback.cpp`
- Create: `tests/test_udp_batch_contract.cpp`
- Modify: `Makefile.core`

**Interface:**

```cpp
using PortableDatagramSend=std::function<UdpSendResult(std::span<const std::uint8_t>)>;
UdpSendBatchResult portable_send_batch(std::span<const std::span<const std::uint8_t>> datagrams,
                                       const PortableDatagramSend& send_one);
```

- [ ] **Step 1: Write RED tests**

Test complete success, `{2,WouldBlock}` after two successful sends, immediate Fatal, empty datagram rejection and no extra callback invocation after failure.

- [ ] **Step 2: Run RED**

Run: `make test-udp-batch-contract`.

- [ ] **Step 3: Implement the generic loop**

No packet aggregation/allocation. Iterate spans, call `send_one`, stop immediately on WouldBlock/Fatal, return exact successful count.

- [ ] **Step 4: Connect POSIX fallback**

macOS/portable platform batch function binds `send_one` to nonblocking `sendmsg()`/`sendto()`. Linux keeps Task 2 native batching.

- [ ] **Step 5: Verify**

Run: `make test-udp-batch-contract test-udp-transport test-direct-video-stress`.

- [ ] **Step 6: Commit**

```bash
git add src/platform/posix/udp_batch_fallback.hpp src/platform/posix/udp_batch_fallback.cpp tests/test_udp_batch_contract.cpp Makefile.core
git commit -m "feat: add portable UDP batch fallback"
```

### Task 4: Make build/install selection Linux/macOS-aware

**Files:**
- Modify: `Makefile.core`
- Modify: `Makefile`
- Create: `tests/test_platform_build_contract.sh`

**Build variables:** `OPAL_OS`, `PLATFORM_SRCS`, `PLATFORM_OBJS`, `PLATFORM_CPPFLAGS`, `PLATFORM_LIBS`, `PLATFORM_REQUIRED_PKGS`.

- [ ] **Step 1: Write dry-run build contract**

The script runs `make -Bn OPAL_OS=linux build/opal` and `make -Bn OPAL_OS=macos build/opal`; Linux output must select PipeWire/Linux batch, macOS output must not probe/link PipeWire, libportal, uinput, systemd or wl-clipboard.

- [ ] **Step 2: Run RED**

Run: `sh tests/test_platform_build_contract.sh`.

- [ ] **Step 3: Add OS selection once**

```make
UNAME_S := $(shell uname -s)
OPAL_OS ?= $(if $(filter Darwin,$(UNAME_S)),macos,$(if $(filter Linux,$(UNAME_S)),linux,unsupported))
ifeq ($(OPAL_OS),unsupported)
$(error Unsupported platform $(UNAME_S); OPAL supports Linux and macOS)
endif
```

Linux selects PipeWire/libportal/libswscale sources, Linux batch and `opal-input`; macOS selects POSIX batch fallback and later `.mm` platform objects.

- [ ] **Step 4: Make install targets platform-specific**

Do not build/install `opal-input`, systemd units or udev rules on macOS. Preserve current Linux install path exactly.

- [ ] **Step 5: Verify Linux**

Run: `sh tests/test_platform_build_contract.sh && make test && make test-hpi`.

- [ ] **Step 6: Commit**

```bash
git add Makefile Makefile.core tests/test_platform_build_contract.sh
git commit -m "build: select Linux and macOS backends"
```

### Task 5: Extract platform system/doctor operations and achieve macOS client build

**Files:**
- Create: `include/opal/platform_system.hpp`
- Create: `src/platform/linux/system_backend.cpp`
- Create: `src/platform/macos/system_backend.mm`
- Modify: `src/system.cpp`
- Modify: `src/setup.cpp`
- Modify: `src/client.cpp`
- Modify: `tests/test_platform_contract.cpp`
- Modify: `Makefile.core`

**Interface:**

```cpp
struct PlatformDoctorItem { std::string name; bool ok=false; std::string detail; };
struct PlatformDoctorInfo {
    std::string platform;
    std::vector<PlatformDoctorItem> items;
    std::vector<PlatformError> errors;
};
PlatformDoctorInfo platform_doctor_info();
int platform_host_service(bool enable);
int platform_restart_services();
void platform_clean_services();
```

- [ ] **Step 1: Extend RED architecture scan**

Require generic `src/system.cpp` to contain no `pipewire`, `systemctl`, `/dev/uinput`, `WAYLAND_DISPLAY` or Linux encoder-name probing.

- [ ] **Step 2: Move Linux operations**

Move PipeWire compiled status, Wayland clipboard status, Pulse/PipeWire service status, uinput status, graphical-environment import, systemd service enable/restart/clean and Linux encoder preference diagnostics into `system_backend.cpp`.

- [ ] **Step 3: Add macOS diagnostics backend**

Report SDL3, linked H.264 decoder, ScreenCaptureKit/VideoToolbox compile support and Screen Recording/Accessibility permission states. Service methods return a stable Unsupported error path; do not invoke systemd/Linux package tooling.

- [ ] **Step 4: Keep generic doctor/network/config logic in `system.cpp`**

Print `platform=linux|macos`, generic identity/network/Tailscale status, then platform items/errors.

- [ ] **Step 5: Verify Linux and Apple Silicon compile milestone**

Linux: `make test-platform-contract test && make test-hpi`.

macOS: `make clean && make -j"$(sysctl -n hw.ncpu)"`; `build/opal doctor`; start client mode and reach SDL initialization without Linux dependency checks.

- [ ] **Step 6: Commit**

```bash
git add include/opal/platform_system.hpp src/system.cpp src/setup.cpp src/client.cpp src/platform/linux/system_backend.cpp src/platform/macos/system_backend.mm tests/test_platform_contract.cpp Makefile.core
git commit -m "refactor: isolate platform system integration"
```

### Task 6: Add native clipboard backends

**Files:**
- Create: `include/opal/clipboard_backend.hpp`
- Create: `src/platform/linux/clipboard_backend.cpp`
- Create: `src/platform/macos/clipboard_backend.mm`
- Create: `tests/test_clipboard_backend.cpp`
- Modify: `src/client.cpp`
- Modify: `src/host.cpp`
- Modify: `Makefile`
- Modify: `Makefile.core`

**Interface:**

```cpp
class ClipboardBackend {
public:
    virtual ~ClipboardBackend()=default;
    virtual bool read_text(std::string&)=0;
    virtual bool write_text(std::string_view)=0;
    virtual std::uint64_t generation() const=0;
    virtual std::string backend_name() const=0;
    virtual PlatformError error() const=0;
};
std::unique_ptr<ClipboardBackend> make_clipboard_backend();
```

- [ ] **Step 1: Write fake-backend RED test**

Prove local change -> `ClipboardSender`, remote completion -> `write_text`, `note_remote_applied` prevents echo, and unchanged content does not queue duplicate control messages.

- [ ] **Step 2: Implement Linux backend**

Move local `wl-copy`/`wl-paste` access out of orchestration; leave `ClipboardSender`/`ClipboardReceiver` wire protocol unchanged.

- [ ] **Step 3: Implement macOS backend**

Use `[NSPasteboard generalPasteboard]`, `NSPasteboardTypeString`, `changeCount`, UTF-8 conversion, `clearContents`, `setString:forType:`. Map failures to `PlatformError`.

- [ ] **Step 4: Wire both roles to the factory**

Client/host use only `ClipboardBackend`. Debug output reports `wl-clipboard` or `nspasteboard` only when active.

- [ ] **Step 5: Verify**

Run: `make test-clipboard test-clipboard-backend test-peer-session`.

Real acceptance: macOS client -> Linux host, clipboard both directions, no echo loop.

- [ ] **Step 6: Commit**

```bash
git add include/opal/clipboard_backend.hpp src/platform/linux/clipboard_backend.cpp src/platform/macos/clipboard_backend.mm src/client.cpp src/host.cpp tests/test_clipboard_backend.cpp Makefile Makefile.core
git commit -m "feat: add native clipboard backends"
```

### Task 7: Extract host input injection and add CGEvent backend

**Files:**
- Create: `include/opal/input_backend.hpp`
- Create: `src/platform/linux/input_backend.cpp`
- Create: `src/platform/macos/input_backend.mm`
- Create: `tests/test_input_backend.cpp`
- Modify: `src/host.cpp`
- Modify: `src/input_helper.cpp`
- Modify: `Makefile`
- Modify: `Makefile.core`

**Interface:**

```cpp
class InputBackend {
public:
    virtual ~InputBackend()=default;
    virtual bool key(WireKeyCode,bool down)=0;
    virtual bool pointer_relative(int dx,int dy)=0;
    virtual bool pointer_absolute(std::uint16_t x,std::uint16_t y)=0;
    virtual bool button(int,bool down)=0;
    virtual bool wheel(int dx,int dy)=0;
    virtual void release_all()=0;
    virtual bool authorized() const=0;
    virtual std::string backend_name() const=0;
    virtual PlatformError error() const=0;
};
std::unique_ptr<InputBackend> make_input_backend();
```

- [ ] **Step 1: Write RED parser/backend test**

Feed `KEY 30 1`, `MOUSE 5 -2`, `POINTER 32768 65535`, `BUTTON 1 1`, `WHEEL 0 -1` through host control parsing with a fake backend; assert exact calls and disconnect -> `release_all()`.

- [ ] **Step 2: Move Linux uinput behind backend**

`<linux/uinput.h>` and `/dev/uinput` stay only in Linux implementation/helper. Preserve current mouse/sensitivity behavior during extraction.

- [ ] **Step 3: Implement full macOS wire-key mapping**

Map every `wire_key::*` emitted by Task 1 to macOS virtual keycodes. Use CGEvent keyboard/mouse/scroll posting. Maintain held keys/buttons and synthesize releases in `release_all()`.

- [ ] **Step 4: Enforce Accessibility authorization**

`authorized()` uses macOS Accessibility trust state. Missing permission returns `PlatformFailure::PermissionDenied`; never silently drop injected input.

- [ ] **Step 5: Verify**

Run: `make test-input test-input-backend test-peer-session`.

Real macOS host: Linux client keyboard, relative/absolute pointer, buttons and wheel after granting Accessibility.

- [ ] **Step 6: Commit**

```bash
git add include/opal/input_backend.hpp src/platform/linux/input_backend.cpp src/platform/macos/input_backend.mm src/host.cpp src/input_helper.cpp tests/test_input_backend.cpp Makefile Makefile.core
git commit -m "feat: add native host input backends"
```

### Task 8: Extract raw capture and persistent encoder interfaces from Linux PipeWire path

**Files:**
- Create: `include/opal/native_video_frame.hpp`
- Create: `include/opal/capture_backend.hpp`
- Create: `include/opal/video_encoder_backend.hpp`
- Create: `src/platform/linux/capture_backend.cpp`
- Create: `src/platform/linux/video_encoder_backend.cpp`
- Create: `tests/test_capture_backend.cpp`
- Modify: `include/opal/pipewire_capture.hpp`
- Modify: `src/pipewire_capture.cpp`
- Modify: `src/video_capture.cpp`
- Modify: `Makefile`

**Interfaces:**

```cpp
enum class NativeVideoFrameKind : std::uint8_t { Cpu, Native };
struct NativeVideoFrame {
    NativeVideoFrameKind kind=NativeVideoFrameKind::Cpu;
    int width=0,height=0,stride=0;
    std::uint32_t pixel_format=0;
    std::uint64_t capture_time_us=0;
    std::span<const std::uint8_t> bytes{};
    void* native_handle=nullptr;
    std::shared_ptr<void> native_owner{};
};
class CaptureBackend {
public:
    virtual ~CaptureBackend()=default;
    virtual bool start(const StreamOptions&)=0;
    virtual bool next(NativeVideoFrame&,int timeout_ms)=0;
    virtual CaptureTimestampQuality timestamp_quality() const=0;
    virtual std::string backend_name() const=0;
    virtual PlatformError error() const=0;
    virtual void stop()=0;
};
class VideoEncoderBackend {
public:
    virtual ~VideoEncoderBackend()=default;
    virtual bool start(const StreamOptions&,int bitrate_kbps)=0;
    virtual bool encode(const NativeVideoFrame&,EncodedMediaUnit&)=0;
    virtual void request_idr()=0;
    virtual bool set_bitrate(int kbps)=0;
    virtual MediaConfig config() const=0;
    virtual std::string backend_name() const=0;
    virtual PlatformError error() const=0;
    virtual void stop()=0;
};
std::unique_ptr<CaptureBackend> make_capture_backend();
std::unique_ptr<VideoEncoderBackend> make_video_encoder_backend();
```

- [ ] **Step 1: Write fake composition RED test**

Verify capture timestamp reaches `EncodedMediaUnit`, one encoder instance encodes multiple frames, bitrate update does not recreate it, `request_idr()` affects the next encode, and capture queue replacement stays latest-first.

- [ ] **Step 2: Extract PipeWire raw capture**

Move portal/session/stream/dequeue/latest-buffer/timestamp code from `NativePipeWireVideoCapture::Impl` into Linux `CaptureBackend`. Preserve current exact/estimated timestamp labeling and bounded raw queue.

- [ ] **Step 3: Extract existing persistent FFmpeg encoder**

Move `EncoderState`, VAAPI/CPU candidate selection, low-delay/no-B-frame configuration, SPS/PPS extraction and access-unit output into Linux `VideoEncoderBackend` with no encoder ordering change.

- [ ] **Step 4: Keep `VideoCapture` facade stable**

Compose capture+encoder internally while retaining current `VideoCapture` public encoded-media API used by `VideoSender`. Do not change wire/media packetization in this task.

- [ ] **Step 5: Verify Linux HPI**

Run: `make test-capture-backend test-video-capture test-video-decoder test-direct-video-pipeline test-hpi`.

- [ ] **Step 6: Commit**

```bash
git add include/opal/native_video_frame.hpp include/opal/capture_backend.hpp include/opal/video_encoder_backend.hpp include/opal/pipewire_capture.hpp src/pipewire_capture.cpp src/video_capture.cpp src/platform/linux/capture_backend.cpp src/platform/linux/video_encoder_backend.cpp tests/test_capture_backend.cpp Makefile
git commit -m "refactor: split native capture and encoder backends"
```

### Task 9: Add ScreenCaptureKit macOS capture

**Files:**
- Create: `src/platform/macos/capture_backend.mm`
- Create: `tests/test_macos_capture_contract.cpp`
- Modify: `src/platform/macos/system_backend.mm`
- Modify: `Makefile.core`

- [ ] **Step 1: Write RED source/lifecycle contract**

Require `SCStream`, `CMSampleBuffer`, `CVPixelBuffer`, one latest-frame slot and structured permission errors. On macOS, compile and exercise permission-denied initialization without crashing.

- [ ] **Step 2: Implement ScreenCaptureKit stream**

Use `SCShareableContent`, selected display, `SCStreamConfiguration` requested dimensions/fps, minimal queue depth and a serial callback queue. Callback replaces any unconsumed frame instead of appending.

- [ ] **Step 3: Preserve native buffer lifetime without RGB copy**

Retain the `CVPixelBufferRef`; set `native_handle` to it and `native_owner` to a `shared_ptr<void>` with `CFRelease` deleter. Encoder ownership of the copied `NativeVideoFrame` keeps the buffer alive through encode.

- [ ] **Step 4: Preserve capture timestamp**

Convert valid sample PTS to OPAL monotonic microseconds using one startup anchor and mark Exact; use local monotonic only when PTS is invalid and mark Estimated.

- [ ] **Step 5: Verify real capture**

On Apple Silicon: Screen Recording permission, 300+ consecutive frames at 60 Hz target, bounded one-frame pending slot, requested higher refresh passed when supported.

- [ ] **Step 6: Commit**

```bash
git add src/platform/macos/capture_backend.mm src/platform/macos/system_backend.mm tests/test_macos_capture_contract.cpp Makefile.core
git commit -m "feat: add ScreenCaptureKit host capture"
```

### Task 10: Add persistent VideoToolbox H.264 encoder

**Files:**
- Create: `src/platform/macos/video_encoder_backend.mm`
- Create: `tests/test_macos_videotoolbox_contract.cpp`
- Modify: `src/video_capture.cpp`
- Modify: `src/video_sender.cpp`
- Modify: `Makefile.core`

- [ ] **Step 1: Write RED lifecycle contract**

Require exactly one `VTCompressionSessionRef` per active configuration, realtime mode, frame reordering disabled, expected FPS/bitrate, forced-keyframe support and session invalidation only on stop/reconfiguration/fatal failure.

- [ ] **Step 2: Implement persistent session**

Create on first frame dimensions. Set `kVTCompressionPropertyKey_RealTime=true`, `kVTCompressionPropertyKey_AllowFrameReordering=false`, average bitrate, expected frame rate and bounded keyframe interval. Submit Task 9 `CVPixelBufferRef` directly with `VTCompressionSessionEncodeFrame`.

- [ ] **Step 3: Normalize output to existing OPAL H.264 contract**

Convert VideoToolbox length-prefixed NAL units to current Annex-B/access-unit form. Extract SPS/PPS from `CMFormatDescription` into `MediaConfig`. Do not add FLV or a new transport format.

- [ ] **Step 4: Wire feedback without recreation**

`request_idr()` sets force-keyframe option on next frame. `set_bitrate()` changes session bitrate property in place and reports structured failure if rejected.

- [ ] **Step 5: Verify cross-OS video**

macOS host -> Linux client and macOS client; trigger IDR, bitrate reduction and reconnect. Debug output must show active `capture=screencapturekit encoder=videotoolbox`.

- [ ] **Step 6: Commit**

```bash
git add src/platform/macos/video_encoder_backend.mm tests/test_macos_videotoolbox_contract.cpp src/video_capture.cpp src/video_sender.cpp Makefile.core
git commit -m "feat: add VideoToolbox host encoder"
```

### Task 11: Add native host audio backends

**Files:**
- Create: `include/opal/audio_capture_backend.hpp`
- Create: `src/platform/linux/audio_capture_backend.cpp`
- Create: `src/platform/macos/audio_capture_backend.mm`
- Create: `tests/test_audio_capture_backend.cpp`
- Modify: `src/video_capture.cpp`
- Modify: `Makefile`
- Modify: `Makefile.core`

**Interface:**

```cpp
class AudioCaptureBackend {
public:
    virtual ~AudioCaptureBackend()=default;
    virtual bool start()=0;
    virtual bool next(EncodedMediaUnit&,int timeout_ms)=0;
    virtual MediaConfig config() const=0;
    virtual std::string backend_name() const=0;
    virtual PlatformError error() const=0;
    virtual void stop()=0;
};
std::unique_ptr<AudioCaptureBackend> make_audio_capture_backend();
```

- [ ] **Step 1: Write fake-backend RED test**

Verify independent audio polling, zero-time audio poll cannot stall video, timestamps/config revision survive, and restart cleanly stops old backend.

- [ ] **Step 2: Move Linux audio-only path behind backend**

Preserve current PulseAudio/PipeWire-compatible capture and AAC config/packet semantics.

- [ ] **Step 3: Implement macOS native audio**

Use ScreenCaptureKit system-audio output on the supported macOS target so audio and screen capture share the closest available clock. Encode in-process with one persistent AAC encoder context if conversion is required. No FFmpeg subprocess.

- [ ] **Step 4: Verify**

Run: `make test-audio-capture-backend test-audio-output test-direct-video-pipeline`.

Real macOS host: audio reaches Linux/macOS clients without degrading video cadence; audio callback never sends network packets directly.

- [ ] **Step 5: Commit**

```bash
git add include/opal/audio_capture_backend.hpp src/platform/linux/audio_capture_backend.cpp src/platform/macos/audio_capture_backend.mm tests/test_audio_capture_backend.cpp src/video_capture.cpp Makefile Makefile.core
git commit -m "feat: add native host audio backends"
```

### Task 12: Finish diagnostics, docs and cross-platform acceptance gates

**Files:**
- Create: `tests/test_cross_platform_architecture.cpp`
- Modify: `tests/test_platform_contract.cpp`
- Modify: `src/system.cpp`
- Modify: `src/platform/linux/system_backend.cpp`
- Modify: `src/platform/macos/system_backend.mm`
- Modify: `README`
- Modify: `Makefile`
- Modify: `Makefile.core`

- [ ] **Step 1: Add final public-interface architecture scan**

Recursively scan `include/opal/` and fail on `<linux/`, `sockaddr_storage`, `socklen_t`, `CGEventRef`, `CVPixelBufferRef`, `CMSampleBufferRef`, `SCStream`, `VTCompressionSessionRef`, `NSPasteboard` and Windows `HANDLE`. Native types are allowed only under `src/platform/`.

- [ ] **Step 2: Finish active-backend doctor output**

Examples only when actually active:

```text
OPAL platform=linux role=host capture=pipewire-native encoder=h264_vaapi input=uinput clipboard=wl-clipboard
OPAL platform=macos role=client presenter=sdl3 decoder=libavcodec clipboard=nspasteboard
OPAL platform=macos role=host capture=screencapturekit encoder=videotoolbox input=cgevent clipboard=nspasteboard
```

Report Screen Recording and Accessibility permission failures separately using stable `PlatformFailure` categories.

- [ ] **Step 3: Update minimal README**

Change Linux-only description to Linux + macOS. Keep Linux package instructions. Add concise macOS dependencies/build command, Apple Silicon tested target, Screen Recording/Accessibility requirements, and state Windows is not supported yet.

- [ ] **Step 4: Linux final verification**

```bash
make clean
make -j"$(nproc)"
make test
make test-hpi
make test-sanitize
```

Supported tests must pass; sanitizer probes may only skip through their existing explicit unsupported-runtime checks.

- [ ] **Step 5: Apple Silicon final verification**

```bash
make clean
make -j"$(sysctl -n hw.ncpu)"
make test
build/opal doctor
```

Run real sessions: macOS client -> Linux host, Linux client -> macOS host, macOS client -> macOS host, Linux client -> Linux host. In each verify video, keyboard, pointer/buttons/wheel, clipboard, audio, reconnect, forced IDR and direct LAN. Exercise relay fallback in at least one cross-OS direction.

- [ ] **Step 6: Compare latency telemetry**

Record capture->encode, encode->send, network, decode and presenter submission/return metrics for Linux before/after and macOS. Any material Linux direct-LAN regression blocks completion and must be fixed in the measured stage rather than by removing Linux native fast paths.

- [ ] **Step 7: Commit**

```bash
git add README Makefile Makefile.core src/system.cpp src/platform/linux/system_backend.cpp src/platform/macos/system_backend.mm tests/test_platform_contract.cpp tests/test_cross_platform_architecture.cpp
git commit -m "docs: finalize Linux macOS platform support"
```

## Execution Order and Review Gates

Execute Tasks 1-12 strictly in order. Tasks 1-5 establish portable core/build/client support; do not start macOS host media until the Apple Silicon client build works. Tasks 6-7 complete cross-platform clipboard/input. Task 8 must pass Linux `make test-hpi` before Tasks 9-11 add macOS host media. Do not combine Tasks 8-10 into a flag-day rewrite. Task 12 starts only after both host paths function.
