# SDL3 Client Platform Design

## Goal

Replace OPAL's direct X11/GLX/XInput2 client platform layer with SDL3 so the same client binary works natively on Wayland and X11 without GLFW, while preserving the existing low-latency decoder, OpenGL YUV rendering path, input semantics, and direct-video architecture.

## Requirements

- SDL3 is the only client window/input platform abstraction.
- Do not add GLFW.
- Do not add direct Wayland client code.
- Remove direct X11/XInput2 use from client presentation/input code once the migration is complete.
- Keep OpenGL for YUV upload/composition; SDL3 owns the window and GL context.
- SDL video/window/context/event work runs on the application main thread.
- Decoder/reassembly/network work remains off the main thread.
- Do not add an extra decoded-frame copy. Transfer decoded frames with FFmpeg frame references.
- Keep at most one decoded frame waiting for presentation. Newer frames replace stale queued frames.
- Preserve Ctrl+Alt+Shift+W release/reacquire and Ctrl+Alt+Shift+Q quit behavior.
- Preserve pointer, button, wheel, keyboard, fullscreen/windowed, aspect-ratio, and zero-vsync behavior.
- `OPAL_DEBUG=1` must expose the selected SDL video backend and exact receiver/presenter failure reason.
- README remains minimal and lists every required/optional external dependency.

## Root Cause

The current presenter opens X11 directly with `XOpenDisplay()` and GLX. On a native Wayland session with `DISPLAY` unset, presenter creation fails even though encrypted media delivery and H.264 configuration succeed. The client input loop is also directly tied to X11/XInput2, so replacing only the presenter would leave input unavailable.

SDL3 already selects Wayland or X11 internally. The client should therefore use SDL3 for both presentation platform ownership and local input instead of carrying separate native backends.

## Threading Architecture

SDL3 video APIs, window creation, OpenGL context creation/swap, and event polling are main-thread operations. OPAL therefore moves presentation ownership to `client_connect()`'s main thread instead of calling platform rendering APIs from `VideoReceiver`'s media worker.

Data flow:

1. `PeerSession` receives encrypted media on its transport thread.
2. `VideoReceiver` decrypts, reassembles, and decodes on its existing worker threads.
3. A decoded frame is retained with `av_frame_ref()` into a one-slot latest-frame mailbox.
4. Publishing a new frame replaces and frees any older unpresented frame and notifies the SDL main loop with an SDL user event.
5. The client main thread drains SDL events, translates input into existing OPAL control commands, takes the latest decoded frame, and calls `VideoPresenter`.
6. `VideoPresenter` uploads YUV/NV12 planes with the existing OpenGL shader path and swaps through `SDL_GL_SwapWindow()`.

This adds no full-frame CPU copy and bounds presentation queueing to one frame.

## Client Platform Ownership

`client.cpp` becomes the owner of SDL initialization and shutdown:

- `SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)` before the client session is started.
- `SDL_Quit()` after the session/presenter has been stopped.
- Create the SDL/OpenGL window on the first decoded frame so the actual source dimensions are known.
- Use `SDL_WINDOW_OPENGL` and fullscreen/windowed flags according to existing OPAL behavior.
- Set swap interval to `0`; failure to disable vsync is debug-visible but not fatal.
- Use `SDL_GetCurrentVideoDriver()` for debug telemetry (`wayland`, `x11`, etc.).

No OPAL code selects Wayland/X11 manually. SDL chooses the backend from the environment/platform.

## Presentation API

`include/opal/video_present.hpp` must stop exposing X11 `Window` or including Xlib.

`VideoPresenter` keeps a minimal API:

- `bool open(int source_width, int source_height, bool fullscreen=true)`
- `bool present_borrowed(DecodedVideoView frame)`
- `std::pair<int,int> drawable_size() const`
- `std::size_t pending_frame_count() const`
- `std::uint64_t presented_frames() const`
- `std::string backend_name() const`
- `void close()`

Internally it owns `SDL_Window*`, `SDL_GLContext`, OpenGL shader/program/textures, and the existing scratch buffers used only for unusual FFmpeg strides.

The existing `x11_window()` API is removed.

## Decoded Frame Mailbox

`VideoReceiver` no longer owns `VideoPresenter`.

It exposes an owning latest-frame transfer API through `SessionSupervisor`, for example:

`bool take_latest_video(DecodedVideoFrame& out)`

Rules:

- mailbox capacity is exactly one decoded frame;
- publishing a newer frame frees the older mailbox frame;
- taking transfers ownership to the caller and clears the mailbox;
- shutdown frees any remaining frame;
- frame publish updates decode telemetry;
- presentation telemetry is updated from the main thread after successful presentation through a small receiver/session callback;
- decode failure still requests an IDR as today;
- presenter failures become session/client failures, not decoder failures.

## SDL Input

Replace X11/XInput2 input collection in `client.cpp` with SDL3 events:

- keyboard: `SDL_EVENT_KEY_DOWN` / `SDL_EVENT_KEY_UP` mapped to Linux evdev keycodes;
- pointer motion: `SDL_EVENT_MOUSE_MOTION` using window-relative coordinates;
- buttons: `SDL_EVENT_MOUSE_BUTTON_DOWN` / `SDL_EVENT_MOUSE_BUTTON_UP`;
- wheel: `SDL_EVENT_MOUSE_WHEEL`;
- quit/window close: terminate the client cleanly.

Capture uses SDL relative mouse mode rather than X11 grabs. Release/reacquire semantics stay user-visible exactly as before:

- Ctrl+Alt+Shift+W releases relative mouse capture and held input state;
- clicking the OPAL window reacquires capture;
- Ctrl+Alt+Shift+Q releases held input and quits.

Input commands sent over the network remain unchanged (`KEY`, `POINTER`, `BUTTON`, `WHEEL`).

## Failure Diagnostics

Before a supervisor reconnect/teardown, `OPAL_DEBUG=1` must log the exact reason, including:

- peer session stopped + `PeerSession::last_error()`;
- receiver failure enum;
- presenter/window creation failure + `SDL_GetError()`;
- selected SDL video backend when available.

Examples:

`OPAL presenter=sdl3 video_driver=wayland`

`OPAL generation=1 unhealthy reason=peer-control-timeout`

`OPAL generation=1 unhealthy reason=presenter-open error=<SDL error>`

This prevents future transport, decoder, and display failures from being conflated.

## Build System

The Makefile changes from direct client X11/XInput2 linkage to SDL3:

Required pkg-config modules after migration:

- `sdl3`
- `openssl`
- `gl`
- `libpulse-simple`
- `libavformat`
- `libavcodec`
- `libavutil`
- `libswresample`

Remove direct `x11` and `xi` requirements when no production source includes them.

Tests that intentionally exercise legacy X11-specific behavior must be rewritten for SDL3 rather than keeping X11 as a hidden dependency.

## README Dependency Contract

Keep the extensionless `README` concise. Add a dependency section with three explicit groups so users can tell what OPAL handles and what the OS/user must provide.

### installed/configured by `sudo make install`

- `opal` binary
- `opal-input` helper
- systemd user units
- udev `uinput` rule
- `uinput` module load attempt
- LAN firewall rules when `firewalld` and/or UFW are already present

These are OPAL-owned installation artifacts/actions, not downloaded third-party packages.

### manual required

Build/runtime requirements after SDL migration:

- C++20 compiler
- `make`
- `pkg-config`
- SDL3 runtime + development files
- OpenSSL runtime + development files
- OpenGL/libglvnd runtime + development files
- FFmpeg executable + `libavformat`, `libavcodec`, `libavutil`, `libswresample` development/runtime files
- PulseAudio client libraries (`libpulse`, `libpulse-simple`); PipeWire's PulseAudio compatibility server is valid
- `gpu-screen-recorder` on the host for the preferred low-latency Wayland/X11 capture path

### optional/manual integrations

- Tailscale for tailnet discovery/direct connectivity
- `firewalld` or UFW if automatic LAN firewall rule management is desired
- systemd/udev are expected on supported Linux installs; systems without them require equivalent manual service/device setup

The README must not claim OPAL downloads or package-installs dependencies automatically at launch. Launch-time checks may diagnose missing dependencies, but package installation remains explicit/manual unless a future installer is intentionally added.

## Tests

TDD requirements:

1. Add a failing presenter architecture test asserting no X11/XInput headers/API remain in the public presenter/client path and SDL3 is used.
2. Add a failing latest-frame mailbox test proving capacity-one replacement and ownership cleanup.
3. Add a failing SDL input mapping test covering key, relative motion, buttons, wheel, release-capture, reacquire, and quit chords.
4. Add a failing session diagnostic test proving receiver/peer failure reasons are logged before recovery.
5. Update `test-video-present` to initialize SDL with a dummy/offscreen-capable environment when possible; skip only the actual window smoke portion when no usable SDL video backend exists.
6. Run focused tests, then `make test`, then a full build.
7. Real-machine acceptance on the Fedora Asahi/KDE Wayland client must show `video_driver=wayland`, present at least one frame, keep a stable media generation, and accept keyboard/mouse input with `DISPLAY` unset.
8. X11 acceptance must still work through SDL's X11 backend without direct OPAL Xlib/XInput code.

## Non-Goals

- No GLFW.
- No direct Wayland protocol implementation.
- No SDL renderer rewrite; retain the current OpenGL YUV shader path.
- No transport, rendezvous, encryption, bitrate, capture, or decoder redesign in this migration unless a failing test proves it is required.
- No automatic package-manager invocation from `opal`.
