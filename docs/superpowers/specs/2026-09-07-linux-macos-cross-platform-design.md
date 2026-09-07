# Linux + macOS Cross-Platform Design

## Goal

Make OPAL a native Linux + macOS remote-desktop system without weakening the existing low-latency Linux path, while defining platform boundaries that allow a future Windows backend without another core-architecture rewrite.

The immediate supported targets are:

- Linux client -> Linux host
- macOS client -> Linux host
- Linux client -> macOS host
- macOS client -> macOS host

Windows is intentionally not implemented in this work. The platform interfaces must nevertheless avoid Linux/macOS assumptions that would prevent a later Windows implementation.

## Architectural Decision

OPAL becomes a platform-neutral streaming/session core surrounded by native platform backends.

Do not make the current Linux implementation compile on every OS by spreading `#ifdef __linux__` / `#ifdef __APPLE__` through core code. Platform conditionals belong in backend selection and build wiring only.

The core continues to own:

- pairing and connection codes
- LAN/tailnet/rendezvous discovery
- peer/session supervision
- direct and relay transport semantics
- encryption and authentication
- packetization and reassembly
- FEC and feedback
- bitrate/congestion decisions
- frame ordering/latest-first policy
- latency telemetry
- clipboard protocol
- input protocol
- reconnect/recovery policy

Platform backends own:

- desktop capture
- hardware encode/decode integration where platform-specific
- input injection
- local clipboard access
- host audio capture
- client audio output only when SDL3 cannot provide the required path
- platform-specific datagram acceleration
- presentation only when a native fast path is intentionally selected

## Source Layout

Move toward the following ownership model without performing unrelated file churn:

```text
src/
  core/
    protocol/session/crypto/discovery/relay/media/network logic

  platform/
    common/
      backend interfaces + portable fallbacks

    linux/
      pipewire capture
      uinput injection
      Wayland clipboard integration
      Linux UDP batching
      Linux hardware-media helpers

    macos/
      ScreenCaptureKit capture bridge
      CGEvent input injection
      NSPasteboard clipboard bridge
      macOS audio capture
      VideoToolbox helpers

  client/
    SDL3 client/event/presentation ownership

  host/
    platform-neutral host/session/media orchestration
```

Existing files do not need to be mechanically relocated in one commit. The important requirement is dependency direction: core/host/client orchestration may depend on backend interfaces; backend implementations may depend on OS APIs; core code must not include OS-specific headers.

## Platform Interfaces

Keep interfaces narrow and capability-oriented rather than creating one giant `Platform` object.

### CaptureBackend

Responsibilities:

- start/stop capture
- expose source dimensions/format/refresh information
- deliver newest available frame with capture timestamp
- report exact vs estimated timestamp quality
- expose whether the frame is CPU-backed or native GPU/native-buffer backed

The core must not know about PipeWire `pw_buffer`, DMA-BUF, `CMSampleBuffer`, or `CVPixelBuffer` types.

A backend may expose an opaque native-frame handle through a small tagged/native view used only by compatible encoder backends. A CPU frame view remains the fallback.

### VideoEncoderBackend

Responsibilities:

- persistent encoder lifetime per media generation
- low-delay H.264 encode
- no B-frames
- bounded buffering
- keyframe request support
- bitrate updates when supported without recreation
- emit encoded access units into the existing OPAL packetization path

Linux keeps the existing FFmpeg/hardware codec selection behavior.

macOS prefers VideoToolbox hardware H.264. The implementation may use libavcodec's VideoToolbox integration where that preserves the persistent zero/low-copy path; otherwise use a thin native VideoToolbox bridge. Do not introduce a second packet/media protocol.

### VideoDecoderBackend

The initial macOS client may keep the existing libavcodec decoder path because the SDL3 client is already platform-neutral. Add a VideoToolbox-backed decoder only when it is measurable and does not add frame copies.

Decoder output must preserve the existing one-frame latest-frame mailbox semantics.

### InputBackend

Responsibilities:

- key down/up
- pointer move/absolute positioning as required by the existing protocol
- mouse buttons
- wheel
- release/reset held state on disconnect

Linux implementation: `/dev/uinput`.

macOS implementation: Quartz/CoreGraphics `CGEvent` APIs with required Accessibility authorization.

The network protocol must not expose Linux headers. Existing wire key values remain stable for compatibility, but their numeric definitions become OPAL protocol constants. Linux maps them to evdev directly; macOS maps them to macOS key events. This prevents a protocol break while removing Linux as a compile-time dependency of clients/hosts.

### ClipboardBackend

Responsibilities:

- read local text clipboard
- write remote text clipboard
- expose a change generation/hash so synchronization does not echo indefinitely

Linux keeps the current Wayland clipboard path.

macOS uses `NSPasteboard` through a minimal Objective-C++ bridge.

The clipboard wire protocol remains unchanged.

### AudioCaptureBackend

Linux keeps the current PipeWire/PulseAudio-compatible host capture path.

macOS uses the native CoreAudio/ScreenCaptureKit-supported audio path chosen during implementation based on which gives the lowest stable latency for system audio on the minimum supported macOS version.

Audio packets/protocol remain unchanged.

### DatagramBackend

The transport API exposes batch send/receive semantics without requiring the caller to know whether the OS has native batch syscalls.

Linux:

- retain `sendmmsg()`/`recvmmsg()` fast paths where used
- preserve partial-batch and backpressure handling

macOS/portable fallback:

- use nonblocking UDP sockets
- submit the same frame-local datagram spans with bounded `sendmsg()`/`sendto()` loops
- preserve partial progress, `WouldBlock`, keyframe/config retry rules and latest-first dropping semantics

Do not emulate `sendmmsg()` with additional packet copies.

Future Windows can implement the same interface with Winsock-specific batching without changing media/session code.

## Client Architecture

SDL3 remains OPAL's cross-platform client platform layer.

For Linux and macOS clients, SDL3 owns:

- window lifecycle
- event processing
- keyboard/mouse collection
- fullscreen/windowed behavior
- client audio output
- baseline video presentation

The existing client input translation is split into two stages:

1. SDL event/scancode -> OPAL wire input value
2. host `InputBackend` -> native OS event

This allows a macOS client to control a Linux host and vice versa without platform-specific input types crossing the network.

Do not introduce Cocoa/AppKit window ownership for the baseline macOS client. A native Metal presenter is a later optimization only if profiling shows the SDL presentation path is material to latency.

## Linux Backend Contract

The Linux port must retain its current behavior and fast paths:

- PipeWire + libportal native capture
- DMA-BUF/import paths where available
- `/dev/uinput` host input
- current Wayland clipboard support
- current audio capture behavior
- FFmpeg/libavcodec media pipeline
- VAAPI/V4L2 M2M/NVENC/QSV selection where available
- Linux UDP batch path
- SDL3 client

The portability refactor must not regress Linux HPI latency gates or convert native paths to generic CPU-copy paths.

## macOS Backend Contract

### Client-first milestone

The first usable macOS milestone is client-only:

- build OPAL on Apple Silicon macOS
- SDL3 window/events/input
- existing encrypted direct/relay session stack
- H.264 decode through libavcodec, with hardware acceleration where available
- SDL3 presentation
- SDL3 audio output
- clipboard through the macOS backend

This milestone must connect to an existing Linux host without requiring a protocol-version fork.

### macOS host capture

Use ScreenCaptureKit for native display capture.

Requirements:

- persistent capture stream
- newest-frame behavior; stale captured frames must not queue deeply
- preserve ScreenCaptureKit timestamps when valid
- no forced RGB conversion when the encoder can consume the native pixel format
- avoid CPU copies between ScreenCaptureKit and VideoToolbox where the platform APIs permit direct `CVPixelBuffer` use
- expose Screen Recording permission failures precisely

### macOS host encoding

Prefer VideoToolbox H.264 hardware encoding on Apple Silicon.

Requirements:

- persistent compression session
- realtime/low-latency configuration
- no B-frame/reordering latency
- bounded frame queue
- keyframe forcing for OPAL recovery
- bitrate changes without encoder recreation when supported
- Annex-B/access-unit normalization only at the boundary needed by the existing OPAL packetizer

Do not shell out to `ffmpeg` for the production native macOS host path.

### macOS input

Use `CGEvent`/Quartz event injection.

Requirements:

- map OPAL wire keys to macOS virtual key semantics
- mouse move/buttons/wheel
- release all held keys/buttons on disconnect/restart
- detect missing Accessibility permission and report it as a host capability failure instead of silently dropping input

### macOS clipboard

Use `NSPasteboard` for text clipboard synchronization.

Keep echo suppression and protocol behavior identical to Linux.

### macOS audio

Use a native low-latency system-audio capture backend. Prefer capture timestamps that can be related to OPAL's media clock. Do not block video/network threads on audio capture.

## Objective-C++ Boundary

Keep Apple framework usage out of normal C++ files.

Use small `.mm` implementation files for APIs that require Objective-C/Apple framework types. Their public headers expose only C++/plain opaque handles owned by OPAL backend classes.

Core code must compile without importing AppKit, CoreGraphics, CoreMedia, CoreVideo, ScreenCaptureKit, or VideoToolbox headers.

## Build System

Keep the existing Makefile-based project.

Introduce OS detection once near the top-level build configuration:

- Linux selects Linux backend sources and pkg-config dependencies
- macOS selects macOS backend sources/frameworks
- unsupported platforms fail with a clear message rather than failing on a random missing Linux header

Linux dependencies remain Linux-only. In particular, macOS builds must not probe or require:

- PipeWire
- libportal
- PulseAudio
- `uinput`
- systemd
- udev
- wl-clipboard
- Linux OpenGL packages

macOS links only the Apple frameworks actually used, plus the shared dependencies needed by the current core/client such as SDL3, OpenSSL and FFmpeg libraries.

Support Apple Silicon first. Do not claim Intel macOS support until it is built and tested; architecture-neutral source should nevertheless be preferred where there is no performance penalty.

## Permissions and Runtime Diagnostics

macOS host mode requires explicit handling for operating-system privacy controls.

At minimum diagnose separately:

- Screen Recording authorization
- Accessibility authorization for input injection
- audio/system-capture authorization if required by the selected API

`opal doctor` must report platform name, backend selections and permission state without conflating permission failures with transport/media failures.

`OPAL_DEBUG=1` should expose lines equivalent to:

```text
OPAL platform=macos role=client presenter=sdl3 decoder=videotoolbox
OPAL platform=macos role=host capture=screencapturekit encoder=videotoolbox input=cgevent clipboard=nspasteboard
```

Only advertise a backend name that is actually active.

## Protocol Compatibility

Do not create separate Linux and macOS protocol variants.

Existing peers must remain interoperable during the migration wherever possible. Any unavoidable protocol extension must be capability-negotiated and backward compatible.

Specifically preserve:

- connection-code format
- peer authentication/encryption
- media packet framing
- control reliability model
- video feedback/IDR recovery
- clipboard message semantics
- input command semantics
- relay/direct behavior

Platform identity/capabilities may be added to diagnostics/negotiation but may not become a requirement for basic connectivity with an older compatible peer.

## Future Windows Constraint

Windows is out of implementation scope.

However, no new public core interface may require:

- POSIX file descriptors as the only socket representation
- Linux evdev types
- Objective-C objects
- Apple pixel-buffer types
- PipeWire types
- Unix process/service APIs

Opaque/native handles and backend-owned implementation state are allowed.

A later Windows port should be able to provide capture, encoder/decoder, input, clipboard, audio, datagram and optional presenter backends without changing the OPAL session/media protocols.

Do not add unused Windows source files, build targets, dependencies or CI jobs in this migration.

## Error Handling

Backend initialization returns structured failure information containing:

- backend/component
- stable failure category
- human-readable OS/library error
- whether fallback is possible

Fallback policy:

- use another native/hardware backend only when it preserves correctness and bounded latency
- never silently fall back from hardware/native capture to a high-latency shell pipeline
- client presentation may fall back within SDL-supported paths
- host startup fails clearly when required capture/input permission or capability is unavailable

A backend failure must not be mislabeled as peer/transport failure.

## Testing

Use TDD for each extraction and backend.

### Architecture contracts

Add tests/source assertions proving:

- core/public headers contain no Linux-only or Apple-only framework headers
- client code contains no direct X11/Wayland/Cocoa input/window dependency outside SDL3
- wire input constants compile without `<linux/input.h>`
- platform backend selection chooses exactly one implementation family per target OS
- Windows remains unimplemented but core interfaces contain no forbidden OS-specific public types

### Linux regression

Before and after extraction run:

- focused unit tests for transport/input/media/clipboard
- `make test`
- `make test-hpi`
- sanitizer targets where supported
- real Linux host/client smoke test

Existing Linux latency/performance behavior is a regression boundary.

### macOS client

On Apple Silicon macOS verify:

- clean build from documented dependencies
- `opal doctor`
- connect to Linux host over LAN
- connect through relay/direct fallback paths already supported by OPAL
- keyboard, pointer, buttons and wheel
- clipboard both directions
- audio playback
- fullscreen/windowed presentation
- reconnect/recovery
- no Linux runtime dependency is probed or required

### macOS host

Verify:

- ScreenCaptureKit permission/setup
- 60 Hz minimum capture and higher-refresh selection when supported by the source/display
- persistent VideoToolbox encode
- Linux client can decode/display the stream
- macOS client can decode/display the stream
- input injection after Accessibility authorization
- clipboard both directions
- audio capture/playback
- keyframe recovery and reconnect
- bounded frame queues under load
- latency telemetry labels exact vs estimated timestamps correctly

Add macOS CI only after the client build is made deterministic enough to be useful. GUI/capture permission tests remain real-machine acceptance tests rather than pretending headless CI validates ScreenCaptureKit.

## Migration Order

1. Extract platform-neutral input key constants and backend interfaces without changing Linux behavior.
2. Extract datagram batching behind a Linux fast path plus portable fallback.
3. Make build configuration select Linux vs macOS sources/dependencies cleanly.
4. Get the existing SDL3 client compiling and running on Apple Silicon macOS.
5. Add macOS clipboard and client input mapping support; verify macOS client -> Linux host.
6. Add ScreenCaptureKit host capture.
7. Add persistent VideoToolbox H.264 host encoding and native-buffer handoff where possible.
8. Add macOS CGEvent input injection.
9. Add macOS host audio capture.
10. Extend `opal doctor`, README/install documentation and platform-specific acceptance tests.
11. Re-profile Linux and macOS end-to-end latency and remove only measured bottlenecks.

Each stage must leave Linux build/tests working. Do not land a large flag-day rewrite.

## Non-Goals

- No Windows implementation in this work.
- No generic lowest-common-denominator capture or media pipeline replacing native fast paths.
- No Electron/web client.
- No GLFW.
- No protocol rewrite.
- No mandatory Metal presenter in the first macOS client milestone.
- No shell-out FFmpeg production capture/encode path on macOS.
- No removal of Linux PipeWire, DMA-BUF, uinput or UDP batching optimizations.
- No unrelated repository restructure.

## Acceptance Definition

This migration is complete when:

1. Linux host/client behavior and latency gates remain healthy.
2. An Apple Silicon macOS client can connect to and fully control a Linux host.
3. A macOS host can be captured, encoded, controlled and clipboard/audio-synchronized from Linux or macOS clients using the same OPAL protocol.
4. Platform-specific headers and APIs are isolated behind backend boundaries.
5. The public/core architecture can accept a later Windows backend without changing the session/media/input/clipboard protocols.
