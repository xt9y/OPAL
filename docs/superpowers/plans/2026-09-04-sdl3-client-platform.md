# SDL3 Client Platform Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace OPAL client-side X11/GLX/XInput2 ownership with SDL3 so native Wayland and X11 sessions use one client path, while preserving the existing low-latency decoder/OpenGL pipeline and input protocol.

**Architecture:** SDL3 owns the client window, OpenGL context, fullscreen state, event queue and capture. `VideoReceiver` decodes off-thread into a one-frame FFmpeg-reference mailbox; `SessionSupervisor` exposes that frame to the client main thread. A thread-safe frame-ready callback pushes one SDL user event so presentation does not poll or add a fixed sleep. Host-side X11 capture/uinput behavior remains unchanged.

**Tech Stack:** C++20, SDL3, OpenGL, FFmpeg/libav*, OpenSSL, PulseAudio client libs, existing OPAL UDP/media stack.

**Spec:** `docs/superpowers/specs/2026-09-04-sdl3-client-platform-design.md`

## Global Constraints

- SDL3 is the only client window/input platform abstraction.
- No GLFW.
- No direct Wayland client code.
- No direct X11/XInput2 use in client presentation/input after migration.
- SDL video/window/context/event pumping stays on the main thread.
- Decoder/reassembly/network remain off-thread.
- At most one decoded frame waits for presentation; replacement uses FFmpeg frame references, not a pixel copy.
- Preserve Ctrl+Alt+Shift+W release/reacquire and Ctrl+Alt+Shift+Q quit behavior.
- Keep swap interval zero and the existing OpenGL YUV420P/NV12 shader path.
- Update README dependency groups and `opal.html`; use the `c.html` NEW! badge treatment for new documentation entries.
- Keep implementation commits minimal to avoid unnecessary workflow runs.

---

### Task 1: SDL3 build and input contract

**Files:**
- Modify: `Makefile`
- Modify: `include/opal/input.hpp`
- Modify: `src/input.cpp`
- Modify: `tests/test_input.cpp`

**Interfaces:**
- Produces: `int linux_keycode_from_sdl_scancode(int scancode)`.
- Produces: client build linked through pkg-config module `sdl3`; direct client `x11`/`xi` requirements removed.

- [ ] Write assertions that client/presenter source contains SDL3 and no client-side X11/XInput2 symbols, and that representative SDL scancodes map to Linux evdev codes.
- [ ] Verify those assertions fail against current `main`.
- [ ] Add SDL3 pkg-config flags/libs and distro dependency hints; remove direct client X11/XInput2 linkage.
- [ ] Implement SDL physical-scancode to Linux evdev mapping for the standard keyboard, modifiers, navigation, keypad and common media keys.
- [ ] Run `make test-input` and the dependency/build-flag tests on a machine with SDL3 development files.

### Task 2: Main-thread SDL presenter

**Files:**
- Modify: `include/opal/video_present.hpp`
- Replace: `src/video_present.cpp`
- Modify: `tests/test_video_present.cpp`

**Interfaces:**
- `VideoPresenter::open(int,int,bool)` creates `SDL_Window` + `SDL_GLContext`.
- `VideoPresenter::present_borrowed(DecodedVideoView)` uploads/draws/swaps.
- `VideoPresenter::backend_name() const` returns `sdl3/<driver>`.
- `VideoPresenter::set_input_capture(bool)` controls SDL relative mouse mode.
- `VideoPresenter::input_captured() const` reports capture state.

- [ ] Change the presenter test to assert the X11 window API is gone and SDL3 backend API exists.
- [ ] Verify it fails against current GLX presenter.
- [ ] Replace XOpenDisplay/GLX window/context/swap handling with SDL3 while retaining the existing GLSL 1.20 YUV/NV12 path, stride-aware uploads, fitted viewport and reusable textures.
- [ ] Use `SDL_GetWindowSizeInPixels`, `SDL_GL_MakeCurrent`, `SDL_GL_SwapWindow`, `SDL_GL_SetSwapInterval(0)` and SDL relative mouse capture.
- [ ] Run `make test-video-present` where a usable SDL video backend exists; make the smoke test skip only when the environment cannot create a window/context.

### Task 3: One-frame decoded mailbox and session bridge

**Files:**
- Modify: `include/opal/video_receiver.hpp`
- Modify: `src/video_receiver.cpp`
- Modify: `include/opal/session.hpp`
- Modify: `src/session.cpp`
- Modify: `tests/test_video_receiver_architecture.cpp`
- Modify: `tests/test_direct_video_pipeline.cpp`

**Interfaces:**
- `VideoReceiver::start[_native](..., std::function<void()> frame_ready)` accepts a notifier.
- `bool VideoReceiver::take_latest_video(DecodedVideoFrame& out)` transfers the single retained frame reference.
- `void VideoReceiver::note_presented(std::int64_t pts_us,double present_ms)` updates presentation telemetry.
- `SessionOptions::video_ready` carries the thread-safe SDL user-event notifier.
- `SessionSupervisor::take_latest_video(...)` and `note_video_presented(...)` bridge the main thread to the receiver.

- [ ] Add failing source/behavior assertions for a capacity-one mailbox and removal of `VideoPresenter`/X11 window ownership from `VideoReceiver`.
- [ ] Verify current architecture fails those assertions.
- [ ] Clone/reference the newest decoded AVFrame into a one-slot mailbox, freeing any superseded unpresented reference and counting it as stale/skipped presentation work.
- [ ] Invoke the frame-ready callback after publish; free pending frame on shutdown.
- [ ] Move presentation telemetry updates to `note_presented`; `media_started()` becomes true once the first decoded frame is publishable so the client can create the window.
- [ ] Remove `presentation_window()` from receiver/session public contracts.
- [ ] Add debug output before supervisor recovery showing peer error vs receiver failure reason.
- [ ] Run receiver/session/direct-media focused tests.

### Task 4: SDL3 client main loop

**Files:**
- Replace client platform portion of: `src/client.cpp`
- Modify: `tests/test_input.cpp`

**Interfaces:**
- `SessionOptions::video_ready` pushes one registered SDL user event using thread-safe `SDL_PushEvent`.
- The main loop waits on SDL events, then drains/takes the newest video frame and presents it.

- [ ] Add failing assertions that `client.cpp` uses SDL events and contains no X11/XInput2 calls.
- [ ] Verify the old client fails them.
- [ ] Initialize SDL video/events before session start, register a frame-ready user event and install the notifier in `SessionOptions`.
- [ ] On first decoded frame, open `VideoPresenter`, report `OPAL presenter=sdl3 video_driver=<driver>` under `OPAL_DEBUG`, and enable capture.
- [ ] Translate SDL key/mouse/button/wheel/close events into existing `KEY`, `MOUSE`, `BUTTON`, `WHEEL` commands; preserve release/reacquire/quit chords and held-state cleanup.
- [ ] Present every newest mailbox frame immediately when awakened; report SDL presenter errors explicitly and stop cleanly instead of triggering opaque reconnect loops.
- [ ] Run input/smoke/session tests.

### Task 5: Dependencies and public documentation

**Files:**
- Modify: `README`
- Modify in `xt9y/xt9y-portfolio`: `opal.html`

- [ ] Keep README terse while adding `installed/configured by sudo make install`, `manual required`, and `optional/manual integrations` groups from the approved spec.
- [ ] Update OPAL architecture/presentation/input/dependency/diagnostic sections in `opal.html` to describe SDL3, native Wayland/X11 backend selection, one-frame mailbox and explicit failure telemetry.
- [ ] Copy the `c.html` sidebar `NEW!` pseudo-element styling and apply it only to new SDL3/platform/dependency/diagnostic entries.
- [ ] Remove stale documentation claiming the client presenter is GLX/X11-only.

### Task 6: Verification

- [ ] Run focused tests: `make test-input test-video-present test-video-receiver-architecture test-peer-session test-udp-transport`.
- [ ] Run `make test` and `make -j"$(nproc)"` on a Linux machine with all manual dependencies.
- [ ] Install and verify on Fedora Asahi/KDE Wayland with `DISPLAY` unset: `OPAL_DEBUG=1 opal` must report SDL video driver `wayland`, show presented frames, retain a stable generation and accept input.
- [ ] Verify an X11 session selects SDL's `x11` backend without any direct OPAL Xlib/XInput2 client code.
