# Linux + macOS Cross-Platform Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make OPAL natively support Linux and Apple Silicon macOS as both client and host while preserving the existing wire protocol and Linux low-latency fast paths, and keep public interfaces suitable for a later Windows backend.

**Architecture:** Keep OPAL's pairing, session, crypto, discovery, relay, packetization, feedback and media-control logic platform-neutral. Extract OS-owned operations behind narrow backend interfaces selected by the build: Linux keeps PipeWire/uinput/Linux UDP batching, while macOS adds SDL3 client support, ScreenCaptureKit, VideoToolbox, CGEvent, NSPasteboard and native system-audio capture. Each extraction lands as a Linux-preserving change before the corresponding macOS backend is added.

**Tech Stack:** C++20, SDL3, OpenSSL, FFmpeg/libavcodec/libavutil/libswresample, Linux PipeWire/libportal/uinput/sendmmsg/recvmmsg, macOS ScreenCaptureKit/CoreMedia/CoreVideo/VideoToolbox/CoreGraphics/AppKit/CoreAudio, Objective-C++ bridges, Make.

**Spec:** `docs/superpowers/specs/2026-09-07-linux-macos-cross-platform-design.md`

## Global Constraints

- Immediate implementation targets are Linux and Apple Silicon macOS only.
- Do not implement Windows source files, targets, dependencies or CI jobs; public interfaces must not prevent a later Windows backend.
- Do not create Linux and macOS protocol variants.
- Preserve connection codes, authentication/encryption, media framing, control reliability, video feedback/IDR recovery, clipboard semantics, input semantics and relay/direct behavior.
- SDL3 remains the baseline client window/event/input/audio-output layer on Linux and macOS.
- Preserve Linux PipeWire + libportal capture, DMA-BUF opportunities, `/dev/uinput`, current hardware encoder selection and Linux UDP batching.
- macOS host capture uses ScreenCaptureKit and production H.264 encoding uses a persistent VideoToolbox session; do not shell out to FFmpeg for the production native macOS host path.
- Apple framework types stay in `.mm` implementation files or private implementation headers; public/core headers contain no AppKit/CoreGraphics/CoreMedia/CoreVideo/ScreenCaptureKit/VideoToolbox types.
- Core/public headers contain no Linux-only headers such as `<linux/input.h>`, `<linux/uinput.h>` or POSIX socket structs as protocol-facing types.
- Keep latest-first bounded frame queues; no portability change may introduce a deep capture/encode/decode/presentation queue.
- Linux `make test`, `make test-hpi` and existing latency behavior are regression boundaries.
- Every behavior change is test-first and each task ends with an independently reviewable commit.

---

### Task 1: Establish platform contracts and platform-neutral wire key codes

**Files:**
- Create: `include/opal/platform.hpp`
- Create: `include/opal/input_wire.hpp`
- Create: `tests/test_platform_contract.cpp`
- Modify: `include/opal/input.hpp`
- Modify: `src/input.cpp`
- Modify: `tests/test_input.cpp`
- Modify: `Makefile.core`

**Interfaces:**
- Produces: `enum class PlatformKind { Linux, MacOS, Unsupported };`
- Produces: `PlatformKind current_platform() noexcept;`
- Produces: `using WireKeyCode = std::uint16_t;`
- Produces: `WireKeyCode wire_keycode_from_sdl_scancode(int scancode);`
- Preserves: existing numeric key values on the wire.

- [ ] **Step 1: Write the failing platform/header contract test**

Add `tests/test_platform_contract.cpp` that reads public headers and asserts they do not contain Linux/Apple framework includes, and verifies protocol key constants are stable:

```cpp
#include <opal/input_wire.hpp>
#include <opal/platform.hpp>
#include <cassert>
#include <fstream>
#include <sstream>
#include <string>

static std::string read_all(const char*path){std::ifstream in(path);std::ostringstream out;out<<in.rdbuf();return out.str();}
int main(){
    static_assert(opal::wire_key::Esc==1);
    static_assert(opal::wire_key::Q==16);
    static_assert(opal::wire_key::W==17);
    static_assert(opal::wire_key::LeftCtrl==29);
    static_assert(opal::wire_key::A==30);
    static_assert(opal::wire_key::LeftShift==42);
    static_assert(opal::wire_key::LeftAlt==56);
    static_assert(opal::wire_key::RightCtrl==97);
    static_assert(opal::wire_key::RightAlt==100);
    static_assert(opal::wire_key::LeftMeta==125);
    static_assert(opal::wire_key::RightMeta==126);
    for(const char*path:{"include/opal/input.hpp","include/opal/input_wire.hpp","include/opal/platform.hpp"}){
        const auto text=read_all(path);
        assert(text.find("<linux/")==std::string::npos);
        assert(text.find("AppKit/")==std::string::npos);
        assert(text.find("CoreGraphics/")==std::string::npos);
    }
}
```

- [ ] **Step 2: Run the new test and verify RED**

Run: `make test-platform-contract`

Expected: FAIL because `input_wire.hpp`, `platform.hpp` and the target do not exist.

- [ ] **Step 3: Add the platform identity and complete OPAL wire-key constants**

`include/opal/platform.hpp`:

```cpp
#pragma once
#include <cstdint>
#include <string_view>
namespace opal {
enum class PlatformKind : std::uint8_t { Linux, MacOS, Unsupported };
constexpr PlatformKind current_platform() noexcept {
#if defined(__linux__)
    return PlatformKind::Linux;
#elif defined(__APPLE__)
    return PlatformKind::MacOS;
#else
    return PlatformKind::Unsupported;
#endif
}
constexpr std::string_view platform_name(PlatformKind p) noexcept {
    return p==PlatformKind::Linux?"linux":p==PlatformKind::MacOS?"macos":"unsupported";
}
}
```

Create `include/opal/input_wire.hpp` with OPAL-owned constants matching the Linux evdev values already transmitted by current releases. Define every key referenced by the existing SDL switch. The required values are:

```cpp
#pragma once
#include <cstdint>
namespace opal {
using WireKeyCode=std::uint16_t;
namespace wire_key {
inline constexpr WireKeyCode None=0, Esc=1, Num1=2, Num2=3, Num3=4, Num4=5, Num5=6, Num6=7, Num7=8, Num8=9, Num9=10, Num0=11;
inline constexpr WireKeyCode Minus=12, Equal=13, Backspace=14, Tab=15, Q=16, W=17, E=18, R=19, T=20, Y=21, U=22, I=23, O=24, P=25;
inline constexpr WireKeyCode LeftBrace=26, RightBrace=27, Enter=28, LeftCtrl=29, A=30, S=31, D=32, F=33, G=34, H=35, J=36, K=37, L=38;
inline constexpr WireKeyCode Semicolon=39, Apostrophe=40, Grave=41, LeftShift=42, Backslash=43, Z=44, X=45, C=46, V=47, B=48, N=49, M=50, Comma=51, Dot=52, Slash=53;
inline constexpr WireKeyCode RightShift=54, KpAsterisk=55, LeftAlt=56, Space=57, CapsLock=58, F1=59, F2=60, F3=61, F4=62, F5=63, F6=64, F7=65, F8=66, F9=67, F10=68;
inline constexpr WireKeyCode NumLock=69, ScrollLock=70, Kp7=71, Kp8=72, Kp9=73, KpMinus=74, Kp4=75, Kp5=76, Kp6=77, KpPlus=78, Kp1=79, Kp2=80, Kp3=81, Kp0=82, KpDot=83;
inline constexpr WireKeyCode F11=87, F12=88, KpEnter=96, RightCtrl=97, KpSlash=98, SysRq=99, RightAlt=100, Home=102, Up=103, PageUp=104, Left=105, Right=106, End=107, Down=108, PageDown=109, Insert=110, Delete=111, Pause=119, LeftMeta=125, RightMeta=126, Compose=127;
}
WireKeyCode wire_keycode_from_sdl_scancode(int scancode);
}
```

- [ ] **Step 4: Replace Linux-header-based SDL mapping and chord logic**

In `src/input.cpp`, remove `<linux/input-event-codes.h>`, rename `linux_keycode_from_sdl_scancode()` to `wire_keycode_from_sdl_scancode()`, and keep the existing SDL-scancode switch while returning `wire_key::*` values. Change the chord checks to `wire_key::LeftCtrl`, `RightCtrl`, `LeftAlt`, `RightAlt`, `LeftShift`, `RightShift`, `Q` and `W`. Keep the command strings unchanged.

Update `include/opal/input.hpp` and `src/client.cpp` call sites to the new function name.

- [ ] **Step 5: Make input tests platform-neutral**

Remove `<linux/input-event-codes.h>` from `tests/test_input.cpp` and assert against `opal::wire_key::*`. Keep all existing pointer normalization, absolute pointer, held-state and chord assertions.

- [ ] **Step 6: Add and run the build target**

Add to `Makefile.core`:

```make
 test-platform-contract: | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_platform_contract.cpp -o $(BUILD)/test-platform-contract
	$(BUILD)/test-platform-contract
```

Run: `make test-platform-contract test-input`

Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add include/opal/platform.hpp include/opal/input_wire.hpp include/opal/input.hpp src/input.cpp src/client.cpp tests/test_platform_contract.cpp tests/test_input.cpp Makefile.core
git commit -m "refactor: make input protocol platform neutral"
```

### Task 2: Remove POSIX socket types from the public transport API while retaining Linux batching

**Files:**
- Create: `include/opal/udp_native.hpp`
- Create: `src/platform/linux/udp_batch.cpp`
- Create: `src/platform/posix/udp_socket.cpp`
- Modify: `include/opal/udp_transport.hpp`
- Modify: `src/udp_transport.cpp`
- Modify: `tests/test_udp_transport.cpp`
- Modify: `Makefile.core`

**Interfaces:**
- Produces: `struct UdpEndpoint { std::array<std::byte,128> native{}; std::uint32_t native_size=0; };`
- Produces: `using UdpNativeHandle=std::uintptr_t;`
- Produces: `struct UdpSocket { UdpNativeHandle handle=kInvalidUdpHandle; std::uint16_t local_port=0; };`
- Produces: `UdpSendBatchResult platform_send_datagrams_batch(UdpNativeHandle,const UdpEndpoint&,std::span<const std::span<const std::uint8_t>>);`
- Preserves: `send_datagrams_batch()` semantics, Linux `sendmmsg()` max batch of 32, partial batch accounting and `WouldBlock` behavior.

- [ ] **Step 1: Change the UDP test to the new public types before implementation**

Remove direct `sockaddr_storage` use from the transport-facing portions of `tests/test_udp_transport.cpp`:

```cpp
auto a=opal::open_udp_socket();
auto b=opal::open_udp_socket();
assert(a.valid()&&b.valid());
opal::UdpEndpoint dst{};
assert(opal::resolve_udp_endpoint("::1",b.local_port,dst));
assert(opal::send_datagram_result(a,dst,payload)==opal::UdpSendResult::Sent);
```

Keep a Linux-only test block inside the test source for socket-buffer/TCLASS verification by converting `a.handle` to `int` only under `#if defined(__linux__)`; this verifies Linux tuning without leaking POSIX types through OPAL headers.

- [ ] **Step 2: Run and verify RED**

Run: `make test-udp-transport`

Expected: compile failure because `UdpEndpoint`, `UdpSocket::valid()` and the new overloads are not implemented.

- [ ] **Step 3: Introduce opaque public endpoint/handle types**

Update `include/opal/udp_transport.hpp` so it includes only standard headers. Add:

```cpp
using UdpNativeHandle=std::uintptr_t;
inline constexpr UdpNativeHandle kInvalidUdpHandle=~UdpNativeHandle{0};
struct UdpEndpoint { std::array<std::byte,128> native{}; std::uint32_t native_size=0; };
struct UdpSocket {
    UdpNativeHandle handle=kInvalidUdpHandle;
    std::uint16_t local_port=0;
    bool valid() const noexcept { return handle!=kInvalidUdpHandle; }
};
struct UdpReceiveSlot {
    std::span<std::uint8_t> buffer{};
    UdpEndpoint source{};
    std::size_t size=0;
    std::uint32_t kernel_drops=0;
};
```

Change all public send/receive/resolve functions to consume `UdpSocket`/`UdpEndpoint`, not `int`, `sockaddr_storage` or `socklen_t`.

- [ ] **Step 4: Isolate POSIX conversion helpers**

`include/opal/udp_native.hpp` is private-to-source/build and may include `<sys/socket.h>`. Provide helpers:

```cpp
int posix_fd(const UdpSocket&) noexcept;
bool endpoint_from_sockaddr(const sockaddr*,socklen_t,UdpEndpoint&);
bool endpoint_to_sockaddr(const UdpEndpoint&,sockaddr_storage&,socklen_t&);
```

Move socket creation, nonblocking setup, DSCP/TCLASS, interface enumeration, endpoint resolution and ordinary send/receive operations into `src/platform/posix/udp_socket.cpp`.

- [ ] **Step 5: Move only native Linux batching into the Linux backend**

Move the existing `sendmmsg()` and `recvmmsg()` code without changing its batching logic into `src/platform/linux/udp_batch.cpp`. Export private helpers used by `src/udp_transport.cpp`.

Run: `make test-udp-transport test-peer-session test-peer-session-relay`

Expected: PASS with Linux still exercising native batching.

- [ ] **Step 6: Commit**

```bash
git add include/opal/udp_transport.hpp include/opal/udp_native.hpp src/udp_transport.cpp src/platform/posix/udp_socket.cpp src/platform/linux/udp_batch.cpp tests/test_udp_transport.cpp Makefile.core
git commit -m "refactor: isolate native UDP transport details"
```

### Task 3: Add the portable/macOS UDP batching fallback

**Files:**
- Create: `src/platform/posix/udp_batch_fallback.cpp`
- Create: `tests/test_udp_batch_contract.cpp`
- Modify: `Makefile.core`

**Interfaces:**
- Consumes: `UdpSocket`, `UdpEndpoint`, `UdpSendBatchResult` from Task 2.
- Produces: same private platform batch symbols as Linux implementation.

- [ ] **Step 1: Add a behavior test for partial progress and empty/invalid batches**

Use injected send callbacks in `tests/test_udp_batch_contract.cpp` to prove the portable loop stops on `WouldBlock`, reports the number already sent, rejects an empty payload and never retries a fatal send.

- [ ] **Step 2: Run and verify RED**

Run: `make test-udp-batch-contract`

Expected: FAIL because the portable batching helper does not exist.

- [ ] **Step 3: Implement fallback batching without packet copies**

In `src/platform/posix/udp_batch_fallback.cpp`, loop directly over the supplied spans and call nonblocking `sendmsg()`/`sendto()` once per datagram. Return `{sent,WouldBlock}` after any transient EAGAIN/EWOULDBLOCK/ENOBUFS, `{sent,Fatal}` on other errors, and `{count,Sent}` only after the complete span is submitted. Do not allocate a combined buffer.

- [ ] **Step 4: Run tests**

Run: `make test-udp-batch-contract test-udp-transport test-direct-video-stress`

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/platform/posix/udp_batch_fallback.cpp tests/test_udp_batch_contract.cpp Makefile.core
git commit -m "feat: add portable UDP batch fallback"
```

### Task 4: Make the Makefile select Linux and macOS backends cleanly

**Files:**
- Modify: `Makefile.core`
- Modify: `Makefile`
- Create: `tests/test_platform_build_contract.sh`

**Interfaces:**
- Produces build variables: `OPAL_OS`, `PLATFORM_SRCS`, `PLATFORM_OBJS`, `PLATFORM_CPPFLAGS`, `PLATFORM_LIBS`, `PLATFORM_REQUIRED_PKGS`.
- Linux selects PipeWire/libportal/uinput/Linux batch implementation.
- macOS selects POSIX batch fallback and Apple framework objects; no Linux dependency probe runs.

- [ ] **Step 1: Add a dry-run build contract test**

Create a shell test that runs `make -Bn OPAL_OS=linux build/opal` and `make -Bn OPAL_OS=macos build/opal`, then asserts Linux output contains PipeWire sources and macOS output does not contain `libpipewire`, `libportal`, `uinput`, `systemd` or `wl-clipboard`.

- [ ] **Step 2: Run RED**

Run: `sh tests/test_platform_build_contract.sh`

Expected: FAIL because current `Makefile` always requires native PipeWire capture.

- [ ] **Step 3: Add one top-level OS selector**

Use:

```make
UNAME_S := $(shell uname -s)
OPAL_OS ?= $(if $(filter Darwin,$(UNAME_S)),macos,$(if $(filter Linux,$(UNAME_S)),linux,unsupported))
ifeq ($(OPAL_OS),unsupported)
$(error Unsupported platform $(UNAME_S); OPAL currently supports Linux and macOS)
endif
```

Linux adds `libportal libpipewire-0.3 libswscale`, `src/pipewire_capture.cpp`, `src/platform/linux/udp_batch.cpp` and the `opal-input` helper. macOS adds `src/platform/posix/udp_batch_fallback.cpp`; Apple framework objects are introduced by later tasks.

- [ ] **Step 4: Make install/test targets platform-aware**

Do not build/install `opal-input`, systemd units or udev rules on macOS. Keep rendezvous-server and platform-neutral unit tests available. Use `sysctl -n hw.ncpu` for documented macOS parallel-build examples rather than `nproc`.

- [ ] **Step 5: Run contracts and Linux regression tests**

Run: `sh tests/test_platform_build_contract.sh && make test && make test-hpi`

Expected: PASS on Linux.

- [ ] **Step 6: Commit**

```bash
git add Makefile Makefile.core tests/test_platform_build_contract.sh
git commit -m "build: select Linux and macOS platform backends"
```

### Task 5: Extract platform diagnostics/services and reach a macOS client build

**Files:**
- Create: `include/opal/platform_system.hpp`
- Create: `src/platform/linux/system_backend.cpp`
- Create: `src/platform/macos/system_backend.mm`
- Modify: `src/system.cpp`
- Modify: `src/setup.cpp`
- Modify: `src/client.cpp`
- Modify: `Makefile.core`
- Test: `tests/test_platform_contract.cpp`

**Interfaces:**
- Produces: `PlatformDoctorInfo platform_doctor_info();`
- Produces: `int platform_host_service(bool enable);`
- Produces: `int platform_restart_services();`
- Produces: `void platform_clean_services();`

- [ ] **Step 1: Extend the architecture contract test**

Assert `src/system.cpp` no longer contains `pipewire`, `systemctl`, `/dev/uinput`, `WAYLAND_DISPLAY` or Linux encoder names after extraction.

- [ ] **Step 2: Run RED**

Run: `make test-platform-contract`

Expected: FAIL against current `system.cpp`.

- [ ] **Step 3: Move Linux-only diagnostics/service actions**

Move current PipeWire compiled check, Wayland clipboard check, Pulse/PipeWire service check, `/dev/uinput` check, graphical-environment import, systemd host service enable/restart/clean behavior and Linux hardware encoder preference strings into `src/platform/linux/system_backend.cpp`.

- [ ] **Step 4: Add the macOS system backend**

`src/platform/macos/system_backend.mm` reports SDL3 availability, linked H.264 decoder availability, ScreenCaptureKit/VideoToolbox compile support and permission states. Host-service operations return a clear unsupported-service result until a deliberate macOS launch-agent design is added; do not invoke `systemctl` or Linux package tooling.

- [ ] **Step 5: Make generic `doctor()` print platform data**

`src/system.cpp` keeps initialization, generic identity/config, Tailscale/network diagnostics and formatting. It prints `platform=<linux|macos>` and delegates native capability lines to `platform_doctor_info()`.

- [ ] **Step 6: Verify Linux plus macOS compile-only milestone**

Linux: `make test-platform-contract test && make test-hpi`.

macOS acceptance command: `make clean && make -j"$(sysctl -n hw.ncpu)"`.

Expected on Apple Silicon: `build/opal` links without PipeWire/uinput/systemd dependencies and client mode reaches SDL initialization.

- [ ] **Step 7: Commit**

```bash
git add include/opal/platform_system.hpp src/system.cpp src/setup.cpp src/client.cpp src/platform/linux/system_backend.cpp src/platform/macos/system_backend.mm Makefile.core tests/test_platform_contract.cpp
git commit -m "refactor: isolate platform system integration"
```

### Task 6: Add native clipboard backends and verify macOS client -> Linux host

**Files:**
- Create: `include/opal/clipboard_backend.hpp`
- Create: `src/platform/linux/clipboard_backend.cpp`
- Create: `src/platform/macos/clipboard_backend.mm`
- Create: `tests/test_clipboard_backend.cpp`
- Modify: `src/client.cpp`
- Modify: `src/host.cpp`
- Modify: `Makefile`
- Modify: `Makefile.core`

**Interfaces:**

```cpp
class ClipboardBackend {
public:
    virtual ~ClipboardBackend()=default;
    virtual bool read_text(std::string&out)=0;
    virtual bool write_text(std::string_view text)=0;
    virtual std::uint64_t generation() const=0;
    virtual std::string backend_name() const=0;
    virtual std::string last_error() const=0;
};
std::unique_ptr<ClipboardBackend> make_clipboard_backend();
```

- [ ] **Step 1: Write a fake-backend synchronization test**

Use an in-memory `ClipboardBackend` in `tests/test_clipboard_backend.cpp` to prove local changes feed `ClipboardSender`, remote completion calls `write_text()`, `note_remote_applied()` suppresses the echo, and empty/unchanged text does not create duplicate traffic.

- [ ] **Step 2: Run RED**

Run: `make test-clipboard-backend`

Expected: missing backend interface.

- [ ] **Step 3: Implement Linux backend without changing protocol classes**

Move Linux local clipboard access out of client/host orchestration. Keep the current `wl-copy`/`wl-paste` behavior on Linux; `ClipboardSender` and `ClipboardReceiver` remain unchanged.

- [ ] **Step 4: Implement macOS `NSPasteboard` bridge**

In `.mm`, use `[NSPasteboard generalPasteboard]`, `NSPasteboardTypeString`, `changeCount`, UTF-8 conversion, and `clearContents` + `setString:forType:`. Store the last observed `changeCount` as `generation()`.

- [ ] **Step 5: Wire client and host to the factory**

Both roles instantiate `make_clipboard_backend()` and use only its C++ interface. `OPAL_DEBUG=1` prints `clipboard=wl-clipboard` or `clipboard=nspasteboard`.

- [ ] **Step 6: Run tests and real cross-OS client acceptance**

Run Linux tests: `make test-clipboard test-clipboard-backend test-peer-session`.

On Apple Silicon, connect macOS client to Linux host and verify text clipboard in both directions with no echo loop.

- [ ] **Step 7: Commit**

```bash
git add include/opal/clipboard_backend.hpp src/platform/linux/clipboard_backend.cpp src/platform/macos/clipboard_backend.mm src/client.cpp src/host.cpp tests/test_clipboard_backend.cpp Makefile Makefile.core
git commit -m "feat: add native clipboard backends"
```

### Task 7: Extract host input injection and add the macOS CGEvent backend

**Files:**
- Create: `include/opal/input_backend.hpp`
- Create: `src/platform/linux/input_backend.cpp`
- Create: `src/platform/macos/input_backend.mm`
- Create: `tests/test_input_backend.cpp`
- Modify: `src/host.cpp`
- Modify: `src/input_helper.cpp`
- Modify: `Makefile`
- Modify: `Makefile.core`

**Interfaces:**

```cpp
class InputBackend {
public:
    virtual ~InputBackend()=default;
    virtual bool key(WireKeyCode code,bool down)=0;
    virtual bool pointer_relative(int dx,int dy)=0;
    virtual bool pointer_absolute(std::uint16_t x,std::uint16_t y)=0;
    virtual bool button(int button,bool down)=0;
    virtual bool wheel(int dx,int dy)=0;
    virtual void release_all()=0;
    virtual bool authorized() const=0;
    virtual std::string backend_name() const=0;
    virtual std::string last_error() const=0;
};
std::unique_ptr<InputBackend> make_input_backend();
```

- [ ] **Step 1: Write parser-to-backend tests**

Feed representative existing control commands (`KEY 30 1`, `MOUSE 5 -2`, `POINTER 32768 65535`, `BUTTON 1 1`, `WHEEL 0 -1`) into host input handling with a fake backend and assert exact calls. Verify disconnect calls `release_all()`.

- [ ] **Step 2: Run RED**

Run: `make test-input-backend`

Expected: interface/injection seam missing.

- [ ] **Step 3: Move Linux uinput injection behind `InputBackend`**

Keep `/dev/uinput` and `<linux/uinput.h>` exclusively in Linux backend/helper implementation. Preserve current 0.3125-era user-visible mouse normalization behavior as it exists at this migration point; do not alter sensitivity semantics during platform extraction.

- [ ] **Step 4: Implement macOS key mapping and event injection**

In `src/platform/macos/input_backend.mm`, map every `wire_key::*` used by the SDL mapping to macOS virtual keycodes. Use `CGEventCreateKeyboardEvent`, `CGEventCreateMouseEvent`/`CGEventPost`, scroll-wheel events, and maintain held-key/button sets so `release_all()` synthesizes releases. `authorized()` uses the Accessibility trust API and missing permission returns `permission-denied` rather than silently succeeding.

- [ ] **Step 5: Run unit/Linux regression and real macOS host input test**

Run: `make test-input test-input-backend test-peer-session`.

On macOS, grant Accessibility permission and verify Linux client keyboard, relative pointer, absolute pointer, buttons and wheel.

- [ ] **Step 6: Commit**

```bash
git add include/opal/input_backend.hpp src/platform/linux/input_backend.cpp src/platform/macos/input_backend.mm src/host.cpp src/input_helper.cpp tests/test_input_backend.cpp Makefile Makefile.core
git commit -m "feat: add native host input backends"
```

### Task 8: Extract the Linux raw-frame/encoder seam without changing Linux output

**Files:**
- Create: `include/opal/capture_backend.hpp`
- Create: `include/opal/video_encoder_backend.hpp`
- Create: `include/opal/native_video_frame.hpp`
- Create: `src/platform/linux/capture_backend.cpp`
- Create: `src/platform/linux/video_encoder_backend.cpp`
- Modify: `src/pipewire_capture.cpp`
- Modify: `include/opal/pipewire_capture.hpp`
- Modify: `src/video_capture.cpp`
- Create: `tests/test_capture_backend.cpp`
- Modify: `Makefile`

**Interfaces:**

```cpp
enum class NativeVideoFrameKind : std::uint8_t { Cpu, Opaque };
struct NativeVideoFrame {
    NativeVideoFrameKind kind=NativeVideoFrameKind::Cpu;
    int width=0,height=0,stride=0;
    std::uint32_t pixel_format=0;
    std::uint64_t capture_time_us=0;
    std::span<const std::uint8_t> bytes{};
    void* opaque=nullptr;
};
class CaptureBackend {
public:
    virtual ~CaptureBackend()=default;
    virtual bool start(const StreamOptions&)=0;
    virtual bool next(NativeVideoFrame&,int timeout_ms)=0;
    virtual void stop()=0;
    virtual CaptureTimestampQuality timestamp_quality() const=0;
    virtual std::string backend_name() const=0;
    virtual std::string last_error() const=0;
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
    virtual std::string last_error() const=0;
    virtual void stop()=0;
};
```

- [ ] **Step 1: Add a fake capture/encoder composition test**

Verify a captured frame's `capture_time_us` reaches the emitted `EncodedMediaUnit`, encoder lifetime spans multiple frames, bitrate update does not recreate the fake encoder, and stale raw frames are replaced rather than accumulating.

- [ ] **Step 2: Run RED**

Run: `make test-capture-backend`

Expected: interfaces absent.

- [ ] **Step 3: Extract existing nested PipeWire `RawFrame` logic**

Move PipeWire stream/portal/dequeue/timestamp logic from `NativePipeWireVideoCapture::Impl` into the Linux `CaptureBackend`. Keep the current latest-buffer drain policy and bounded raw queue.

- [ ] **Step 4: Extract existing persistent FFmpeg encoder state**

Move `EncoderState`, VAAPI/CPU encoder selection, low-delay settings, no-B-frame settings, SPS/PPS extraction and packet emission into the Linux `VideoEncoderBackend`. Preserve current encoder ordering and exact H.264 output contract.

- [ ] **Step 5: Keep `VideoCapture` as the stable encoded-media facade**

`VideoCapture` composes the selected capture + encoder backend and continues exposing the existing `start`, `next_view`, `configs`, `config_revision`, `backend_name`, timestamp quality and error API to `VideoSender`. No sender/protocol change is allowed in this task.

- [ ] **Step 6: Run Linux media gates**

Run: `make test-capture-backend test-video-capture test-video-decoder test-direct-video-pipeline test-hpi`.

Expected: PASS with Linux backend names/timestamp quality unchanged except for deliberately clearer component naming.

- [ ] **Step 7: Commit**

```bash
git add include/opal/capture_backend.hpp include/opal/video_encoder_backend.hpp include/opal/native_video_frame.hpp include/opal/pipewire_capture.hpp src/pipewire_capture.cpp src/video_capture.cpp src/platform/linux/capture_backend.cpp src/platform/linux/video_encoder_backend.cpp tests/test_capture_backend.cpp Makefile
git commit -m "refactor: split native capture and encoder backends"
```

### Task 9: Add ScreenCaptureKit capture on macOS

**Files:**
- Create: `src/platform/macos/capture_backend.mm`
- Create: `tests/test_macos_capture_contract.cpp`
- Modify: `Makefile.core`
- Modify: `src/platform/macos/system_backend.mm`

**Interfaces:**
- Implements `CaptureBackend` from Task 8.
- Opaque frame handle is a backend-retained `CVPixelBufferRef`; its lifetime remains valid until the next successful `next()`/explicit release by the encoder handoff.

- [ ] **Step 1: Add source/ownership contract tests**

On all platforms, source-scan the macOS backend to require `SCStream`, `CMSampleBuffer`, `CVPixelBuffer` and a capacity-one/latest-frame replacement path. On macOS compile the backend test and verify unsupported/missing Screen Recording permission produces a structured failure rather than a crash.

- [ ] **Step 2: Run RED**

Run: `make test-macos-capture-contract`

Expected: missing source/backend.

- [ ] **Step 3: Implement ScreenCaptureKit stream selection**

Use `SCShareableContent`, select the target display, configure `SCStreamConfiguration` with requested width/height/fps, queue depth kept minimal, and a dedicated serial callback queue. In the stream output callback, retain only the newest complete screen frame and release the prior unconsumed buffer.

- [ ] **Step 4: Preserve native timestamps and pixel buffers**

Use the sample presentation timestamp when valid and convert it to OPAL monotonic microseconds with one startup anchor. Set timestamp quality `Exact` for valid capture timestamps and `Estimated` only when falling back to local monotonic time. Return the `CVPixelBufferRef` through `NativeVideoFrame::opaque` without RGB copying.

- [ ] **Step 5: Verify permission and capture acceptance**

On macOS: run `opal doctor`, start host capture after granting Screen Recording, verify at least 300 consecutive frames at 60 Hz target without an unbounded queue, and verify high-refresh requested FPS is passed to ScreenCaptureKit when supported.

- [ ] **Step 6: Commit**

```bash
git add src/platform/macos/capture_backend.mm src/platform/macos/system_backend.mm tests/test_macos_capture_contract.cpp Makefile.core
git commit -m "feat: add ScreenCaptureKit host capture"
```

### Task 10: Add persistent VideoToolbox H.264 encoding and connect the macOS host video path

**Files:**
- Create: `src/platform/macos/video_encoder_backend.mm`
- Create: `tests/test_macos_videotoolbox_contract.cpp`
- Modify: `Makefile.core`
- Modify: `src/video_capture.cpp`
- Modify: `src/video_sender.cpp`

**Interfaces:**
- Implements `VideoEncoderBackend` from Task 8.
- Consumes `CVPixelBufferRef` from Task 9 without a CPU RGB copy.
- Emits existing `EncodedMediaUnit` H.264 access units and `MediaConfig` parameter sets.

- [ ] **Step 1: Write source/lifecycle contract tests**

Assert the macOS implementation owns one `VTCompressionSessionRef`, sets realtime mode, disables frame reordering, uses expected-frame-rate and bitrate properties, implements forced keyframes, and invalidates the session only on stop/reconfiguration/fatal failure.

- [ ] **Step 2: Run RED**

Run: `make test-macos-videotoolbox-contract`

Expected: source/backend absent.

- [ ] **Step 3: Implement persistent VideoToolbox session**

Create the compression session on the first frame dimensions, set `kVTCompressionPropertyKey_RealTime=true`, `kVTCompressionPropertyKey_AllowFrameReordering=false`, target average bitrate, expected frame rate and a bounded keyframe interval. Encode the incoming `CVPixelBufferRef` directly with `VTCompressionSessionEncodeFrame`.

- [ ] **Step 4: Normalize VideoToolbox output at the packetizer boundary**

Convert length-prefixed H.264 NAL units from each encoded sample to the Annex-B/access-unit format currently consumed by OPAL. Extract SPS/PPS from the format description into `MediaConfig`. Do not create an FLV subprocess or a second transport framing format.

- [ ] **Step 5: Wire IDR and bitrate feedback**

`request_idr()` sets `kVTEncodeFrameOptionKey_ForceKeyFrame` for the next submitted frame. `set_bitrate()` updates the VideoToolbox session property in place and returns false only when VideoToolbox rejects the change; it must not recreate the encoder on normal congestion feedback.

- [ ] **Step 6: Cross-platform stream acceptance**

Verify macOS host -> Linux client and macOS host -> macOS client. Trigger an IDR request, bitrate reduction and reconnect. Confirm Linux decodes without protocol/version changes and `OPAL_DEBUG=1` reports `capture=screencapturekit encoder=videotoolbox`.

- [ ] **Step 7: Commit**

```bash
git add src/platform/macos/video_encoder_backend.mm tests/test_macos_videotoolbox_contract.cpp src/video_capture.cpp src/video_sender.cpp Makefile.core
git commit -m "feat: add VideoToolbox host encoder"
```

### Task 11: Add native macOS host audio capture without blocking the video path

**Files:**
- Create: `include/opal/audio_capture_backend.hpp`
- Create: `src/platform/linux/audio_capture_backend.cpp`
- Create: `src/platform/macos/audio_capture_backend.mm`
- Create: `tests/test_audio_capture_backend.cpp`
- Modify: `src/video_capture.cpp`
- Modify: `Makefile`
- Modify: `Makefile.core`

**Interfaces:**

```cpp
class AudioCaptureBackend {
public:
    virtual ~AudioCaptureBackend()=default;
    virtual bool start()=0;
    virtual bool next(EncodedMediaUnit&,int timeout_ms)=0;
    virtual MediaConfig config() const=0;
    virtual std::string backend_name() const=0;
    virtual std::string last_error() const=0;
    virtual void stop()=0;
};
std::unique_ptr<AudioCaptureBackend> make_audio_capture_backend();
```

- [ ] **Step 1: Write queue/lifecycle tests using a fake backend**

Verify audio capture runs independently, never blocks a video `next_view()` call beyond its own zero-time poll, preserves timestamps/config revision, and stops cleanly on session restart.

- [ ] **Step 2: Run RED**

Run: `make test-audio-capture-backend`

Expected: backend interface absent.

- [ ] **Step 3: Preserve Linux behavior behind its backend**

Move the existing PulseAudio/PipeWire-compatible audio-only capture logic out of generic `video_capture.cpp` into the Linux backend without changing AAC packet/config semantics.

- [ ] **Step 4: Implement macOS native system-audio capture**

Use ScreenCaptureKit audio output when available on the supported macOS target so screen video/audio share the closest clock domain. Convert captured PCM to the existing audio format/encoder path in-process; if AAC encoding remains through libavcodec, keep one persistent encoder context. Do not shell out to FFmpeg.

- [ ] **Step 5: Run audio and end-to-end acceptance**

Run: `make test-audio-capture-backend test-audio-output test-direct-video-pipeline`.

On macOS host, verify audio reaches Linux/macOS clients while video frame cadence stays stable and no audio callback performs network sends directly.

- [ ] **Step 6: Commit**

```bash
git add include/opal/audio_capture_backend.hpp src/platform/linux/audio_capture_backend.cpp src/platform/macos/audio_capture_backend.mm tests/test_audio_capture_backend.cpp src/video_capture.cpp Makefile Makefile.core
git commit -m "feat: add native host audio capture backends"
```

### Task 12: Finalize doctor, README, acceptance gates and future-Windows architecture checks

**Files:**
- Modify: `src/system.cpp`
- Modify: `src/platform/linux/system_backend.cpp`
- Modify: `src/platform/macos/system_backend.mm`
- Modify: `README`
- Modify: `Makefile`
- Modify: `Makefile.core`
- Modify: `tests/test_platform_contract.cpp`
- Create: `tests/test_cross_platform_architecture.cpp`

**Interfaces:**
- `opal doctor` reports active platform/backend/permission state.
- Adds aggregate target: `test-cross-platform-architecture`.
- Keeps Windows unimplemented while enforcing no forbidden OS-native public types.

- [ ] **Step 1: Add final architecture assertions**

Scan all `include/opal/*.hpp` and `include/opal/**/*.hpp` and fail on `<linux/`, `sockaddr_storage`, `socklen_t`, `CGEventRef`, `CVPixelBufferRef`, `CMSampleBufferRef`, `SCStream`, `VTCompressionSessionRef`, `NSPasteboard` and `HANDLE`. Permit native types only in `src/platform/**` and the private `udp_native.hpp` source-only helper.

- [ ] **Step 2: Run RED if leakage remains**

Run: `make test-cross-platform-architecture`

Expected: any remaining public platform leakage is reported with a file path.

- [ ] **Step 3: Finish `opal doctor` output**

Required active examples:

```text
OPAL platform=linux role=host capture=pipewire-native encoder=h264_vaapi input=uinput clipboard=wl-clipboard
OPAL platform=macos role=client presenter=sdl3 decoder=libavcodec clipboard=nspasteboard
OPAL platform=macos role=host capture=screencapturekit encoder=videotoolbox input=cgevent clipboard=nspasteboard
```

Report Screen Recording and Accessibility authorization separately. Do not claim VideoToolbox/ScreenCaptureKit is active merely because it was compiled.

- [ ] **Step 4: Update the minimal README**

Change the opening description from Linux-only to Linux + macOS. Keep separate Linux package instructions and a concise macOS dependency/build section using Homebrew-provided SDL3/OpenSSL/FFmpeg plus Apple system frameworks. Document that Windows is not supported yet. Document macOS Screen Recording and Accessibility permissions and Apple Silicon as the tested target.

- [ ] **Step 5: Run Linux final verification**

Run:

```bash
make clean
make -j"$(nproc)"
make test
make test-hpi
make test-sanitize
```

Expected: all supported tests pass or existing sanitizer runtime probes explicitly skip unsupported sanitizer/runtime combinations.

- [ ] **Step 6: Run Apple Silicon final verification**

Run:

```bash
make clean
make -j"$(sysctl -n hw.ncpu)"
make test
opal doctor
```

Then perform four real-session cases: macOS client -> Linux host, Linux client -> macOS host, macOS client -> macOS host, Linux client -> Linux host. For each verify video, keyboard, pointer/buttons/wheel, clipboard, audio, reconnect, forced IDR and direct LAN path. Exercise relay fallback in at least one cross-OS direction.

- [ ] **Step 7: Compare latency/regression telemetry**

Record capture->encode, encode->send, network, decode and presenter submission/return metrics for Linux before/after and macOS. Reject the migration if the Linux direct-LAN path regresses materially from the pre-migration HPI baseline; fix the measured regression rather than replacing native Linux fast paths with generic fallbacks.

- [ ] **Step 8: Commit final docs/gates**

```bash
git add README Makefile Makefile.core src/system.cpp src/platform/linux/system_backend.cpp src/platform/macos/system_backend.mm tests/test_platform_contract.cpp tests/test_cross_platform_architecture.cpp
git commit -m "docs: finalize Linux macOS platform support"
```

## Execution Order and Review Gates

Execute Tasks 1-12 strictly in order. Tasks 1-5 establish portable core/build/client support; after Task 5, verify a macOS client can build before continuing. Tasks 6-7 complete cross-platform clipboard/input. Task 8 is the Linux-preserving media seam and must pass `make test-hpi` before Tasks 9-11 add macOS host media. Task 12 is acceptance only after both host paths function.

Do not batch Tasks 8-10 into one flag-day media rewrite. The Linux extraction must be reviewed and green before ScreenCaptureKit or VideoToolbox is connected.
