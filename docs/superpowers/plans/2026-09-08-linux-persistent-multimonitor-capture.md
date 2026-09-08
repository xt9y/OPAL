# Linux Persistent Multi-Monitor Capture Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep one persistent Linux/Wayland portal capture authorization alive across OPAL media recovery, capture every monitor selected in KDE, compose them as equal-sized horizontal tiles, and map absolute pointer input back to the correct host monitor.

**Architecture:** Split portal/PipeWire ownership from encoding. A process-wide Linux capture hub owns the XDG ScreenCast session, PipeWire remote, selected monitor streams, restore-token rotation, latest-frame mailboxes, composition geometry, and pointer transform. `NativePipeWireVideoCapture` becomes an encoder consumer of composed raw frames, so `VideoSender` restarts no longer recreate the portal session. The existing single-H.264 OPAL media protocol and client decoder remain unchanged.

**Tech Stack:** C++20, libportal/XDG ScreenCast, PipeWire/SPA, FFmpeg libavcodec/libswscale, Linux uinput, existing OPAL media/input code.

**Spec:** `docs/superpowers/specs/2026-09-08-linux-persistent-multimonitor-capture-design.md`

## Global Constraints

- Linux/Wayland only; macOS behavior must remain unchanged.
- Portal source type is monitor-only and selection allows multiple monitors.
- The first deliberate host authorization may show the compositor chooser; media recovery must never create a new portal session.
- Restore tokens remain private under `~/.opal` and are replaced atomically whenever the portal rotates them.
- All selected monitors are displayed as equal-width/equal-height tiles in one horizontal row.
- Relative mouse input and existing relative mouse normalization are unchanged.
- Existing single-H.264 video protocol, encryption, FEC, relay/direct UDP, decoder and presenter contracts are unchanged.
- Missing/new monitors must not silently trigger interactive reauthorization.

---

### Task 1: Pure multi-monitor geometry and pointer transforms

**Files:**
- Create: `include/opal/multimonitor.hpp`
- Create: `src/multimonitor.cpp`
- Create: `tests/test_multimonitor.cpp`
- Modify: `Makefile`

**Interfaces:**
- Produces `opal::MonitorGeometry { int source_width, source_height, logical_x, logical_y, logical_width, logical_height; }`.
- Produces `opal::CompositeLayout build_composite_layout(std::span<const MonitorGeometry>, int max_width, int max_height)`.
- Produces `std::pair<int,int> map_composite_pointer(const CompositeLayout&, int x65535, int y65535)` returning host virtual-desktop normalized coordinates in `[0,65535]`.
- `CompositeLayout` records equal tile dimensions, final canvas dimensions, per-monitor inverse transforms, and logical desktop bounds.

- [ ] **Step 1: Write failing geometry tests**

Cover two mixed-resolution monitors, three monitors, equal tile dimensions, final whole-canvas clamp, logical monitor offsets, and pointer mapping into each tile. Include a case where portal logical position metadata is absent and verify deterministic horizontal fallback.

- [ ] **Step 2: Run the focused test and verify RED**

Run: `make test-multimonitor`
Expected: compile/link failure because `multimonitor.hpp` and the geometry functions do not exist.

- [ ] **Step 3: Implement the minimal pure geometry layer**

Use integer/rational arithmetic for tile boundaries and pointer mapping where possible. Choose the largest selected source width/height as the unbounded tile size, then uniformly clamp the completed horizontal canvas to `max_width/max_height`; preserve identical final tile dimensions. Clamp pointer values and map tile-local normalized coordinates through each monitor's logical rectangle. If logical metadata is unavailable, synthesize horizontal logical rectangles from source dimensions.

- [ ] **Step 4: Run tests and verify GREEN**

Run: `make test-multimonitor`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/opal/multimonitor.hpp src/multimonitor.cpp tests/test_multimonitor.cpp Makefile
git commit -m "feat: add multi-monitor composition geometry"
```

### Task 2: Persistent Linux portal capture hub and atomic token rotation

**Files:**
- Create: `include/opal/linux_capture_session.hpp`
- Create: `src/linux_capture_session.cpp`
- Create: `tests/test_linux_capture_session_contract.cpp`
- Modify: `Makefile`

**Interfaces:**
- Produces `opal::LinuxRawFrame` containing BGRA pixels, dimensions, stride, capture timestamp and source generation.
- Produces process-wide functions:
  - `bool linux_capture_authorize(const StreamOptions&, const std::string& token_file)` for deliberate setup-time interactive authorization.
  - `bool linux_capture_prepare(const StreamOptions&, const std::string& token_file)` for host-service startup/restoration.
  - `bool linux_capture_next(LinuxRawFrame&, int timeout_ms)` for encoder consumers.
  - `bool linux_capture_ready()`.
  - `bool linux_capture_authorization_lost()`.
  - `std::string linux_capture_last_error()`.
  - `CompositeLayout linux_capture_layout()`.
  - `void linux_capture_shutdown()`.
- The hub owns `XdpPortal`, `XdpSession`, the PipeWire remote/core/thread loop, N `pw_stream` monitor streams, per-monitor latest-frame mailboxes and the compositor buffer.

- [ ] **Step 1: Write RED structural/lifecycle contracts**

Require `XDP_SCREENCAST_FLAG_MULTIPLE`, iteration over every child returned by `xdp_session_get_streams`, atomic restore-token replacement (`temporary file -> fsync/close -> rename`), and one process-wide portal owner. Assert that the runtime frame-consumer API contains no interactive-authorization parameter.

- [ ] **Step 2: Run contract test and verify RED**

Run: `make test-linux-capture-session-contract`
Expected: FAIL because the hub does not exist.

- [ ] **Step 3: Move portal/PipeWire ownership into the hub**

Create the ScreenCast session with monitor-only source type, embedded cursor, persistent mode and `XDP_SCREENCAST_FLAG_MULTIPLE`. Parse every returned stream child, including optional `position`, `size`, `id` and `pipewire-serial` metadata when present. Open one PipeWire remote and one `pw_stream` per selected node. Store only the newest frame per monitor; do not queue historical frames.

- [ ] **Step 4: Implement token rotation safely**

After every successful `Start`, read the new restore token and atomically replace the token file. Never truncate the old token before the new token is durably written. Mark failed/closed full portal sessions as authorization lost rather than automatically creating a fresh interactive session from consumer code.

- [ ] **Step 5: Implement multi-monitor composition**

Use `build_composite_layout`, reusable libswscale contexts and reusable BGRA buffers. Scale each latest monitor image into its equal-sized tile and write tiles horizontally into one composed frame. Use the oldest contributing capture timestamp as the composite timestamp. Reuse the newest previous monitor image when refresh rates differ.

- [ ] **Step 6: Run focused tests/contracts**

Run: `make test-linux-capture-session-contract test-multimonitor`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add include/opal/linux_capture_session.hpp src/linux_capture_session.cpp tests/test_linux_capture_session_contract.cpp Makefile
git commit -m "feat: add persistent Linux capture session"
```

### Task 3: Make native Linux H.264 encoding consume the persistent hub

**Files:**
- Modify: `include/opal/pipewire_capture.hpp`
- Modify: `src/pipewire_capture.cpp`
- Modify: `src/video_capture.cpp`
- Create: `tests/test_linux_capture_restart_contract.cpp`

**Interfaces:**
- Consumes `linux_capture_next()` and `LinuxRawFrame` from Task 2.
- `NativePipeWireVideoCapture::start()` starts/restarts only encoder state; it does not create `XdpPortal`, `XdpSession`, PipeWire remote or monitor streams.
- `NativePipeWireVideoCapture::stop()` stops only its encoder consumer.

- [ ] **Step 1: Write RED restart contract**

Assert that `pipewire_capture.cpp` no longer contains `xdp_portal_create_screencast_session` or `xdp_session_start`, and that encoder restart paths call the hub consumer rather than portal creation.

- [ ] **Step 2: Run and verify RED**

Run: `make test-linux-capture-restart-contract`
Expected: FAIL against the current portal-owning `NativePipeWireVideoCapture`.

- [ ] **Step 3: Refactor `NativePipeWireVideoCapture`**

Delete portal/PipeWire stream ownership from this encoder wrapper. Keep the persistent FFmpeg encoder selection/configuration code, but feed it composed `LinuxRawFrame` values from `linux_capture_next()`. Encoder reconfiguration on size/bitrate changes must leave the hub untouched.

- [ ] **Step 4: Make native fallback errors explicit**

When the hub reports authorization lost, propagate a stable capture error that says Linux screen authorization must be redone. Do not fall back to an external portal capture command because that would reopen a chooser. Non-Wayland/non-native builds keep their existing fallback behavior.

- [ ] **Step 5: Run focused tests**

Run: `make test-linux-capture-restart-contract test-multimonitor`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add include/opal/pipewire_capture.hpp src/pipewire_capture.cpp src/video_capture.cpp tests/test_linux_capture_restart_contract.cpp
git commit -m "refactor: decouple Linux capture from encoder restarts"
```

### Task 4: Establish capture authorization before remote media sessions

**Files:**
- Modify: `src/setup.cpp`
- Modify: `src/host.cpp`
- Modify: `include/opal/host.hpp` if a small setup helper declaration is needed
- Modify: `tests/test_setup.cpp`
- Create: `tests/test_host_capture_lifecycle_contract.cpp`

**Interfaces:**
- Consumes `linux_capture_authorize()` during deliberate Linux host setup.
- Consumes `linux_capture_prepare()` during Linux host daemon startup before accepting/serving client media sessions.
- Host config gains `host.remember_screens=true|false`.

- [ ] **Step 1: Add RED setup tests**

Extend setup tests so Linux host setup asks `Remember selected screens for automatic hosting? [Y/n]`, stores the choice, and invokes deliberate screen authorization only when enabled.

- [ ] **Step 2: Add RED host lifecycle contract**

Assert `host_run`/`host_daemon` prepares the capture hub before peer media serving and that `VideoSender`/peer callbacks do not invoke portal authorization.

- [ ] **Step 3: Implement the setup prompt and saved preference**

Use the existing `ask_yes_no` pattern adjacent to Wake-on-LAN. For remembered screens, call `linux_capture_authorize` with `~/.opal/portal-session.token` and fail setup with a clear message if the user denies or the portal fails. Leave the option off when the user answers no.

- [ ] **Step 4: Prepare the hub at host-service startup**

If Wayland + remembered screens are enabled, restore/start the persistent hub before accepting a remote media session. If preparation fails, keep host control/network service alive but report capture authorization unavailable; do not open an interactive chooser from a client-triggered path.

- [ ] **Step 5: Run tests**

Run: `make test-setup test-host-capture-lifecycle-contract`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/setup.cpp src/host.cpp include/opal/host.hpp tests/test_setup.cpp tests/test_host_capture_lifecycle_contract.cpp
git commit -m "feat: authorize Linux screen capture during host setup"
```

### Task 5: Map combined-canvas absolute pointer input back to host monitors

**Files:**
- Modify: `src/host.cpp`
- Modify: `src/input.cpp` only if a reusable parser/helper is needed
- Modify: `tests/test_input.cpp`
- Create: `tests/test_multimonitor_input.cpp`

**Interfaces:**
- Consumes `linux_capture_layout()` and `map_composite_pointer()`.
- Existing `MOUSE dx dy` relative events are forwarded byte-for-byte/semantically unchanged.
- `POINTER x y` remains normalized to `[0,65535]` on the wire; host remaps those values before sending them to Linux uinput.

- [ ] **Step 1: Write RED pointer tests**

Cover pointer positions in monitor 0/1/2 tiles, mixed logical monitor offsets, tile-edge clamping, and unchanged relative mouse commands.

- [ ] **Step 2: Run and verify RED**

Run: `make test-multimonitor-input`
Expected: FAIL because host pointer input is still forwarded directly.

- [ ] **Step 3: Implement host-side pointer transform**

When a Linux multi-monitor layout is active, parse `POINTER x y`, map through the current layout, and forward the remapped normalized pointer command to `opal-input`. For single monitor or unavailable geometry, preserve existing behavior. Do not modify `MOUSE`, wheel, keyboard or button paths.

- [ ] **Step 4: Run pointer/input tests**

Run: `make test-multimonitor-input test-input`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/host.cpp src/input.cpp tests/test_input.cpp tests/test_multimonitor_input.cpp
git commit -m "feat: map input across composed monitors"
```

### Task 6: Recovery, monitor removal, diagnostics and documentation

**Files:**
- Modify: `src/linux_capture_session.cpp`
- Modify: `src/video_sender.cpp`
- Modify: `src/video_capture.cpp`
- Modify: `README`
- Create: `tests/test_linux_capture_recovery_contract.cpp`

**Interfaces:**
- Capture hub increments a layout/source generation when selected streams disappear or geometry changes.
- Encoder consumer observes generation/size changes and rebuilds encoder configuration + requests/produces an IDR without recreating the portal.
- Authorization loss is terminal for capture until explicit reauthorization.

- [ ] **Step 1: Write RED recovery contracts**

Require monitor removal to update the layout without portal recreation, bitrate/IDR/capture-stale recovery to leave the hub alive, and authorization-lost diagnostics to name host screen reauthorization rather than generic capture fallback.

- [ ] **Step 2: Implement monitor stream loss handling**

Mark only the failed monitor stream unavailable, rebuild the layout from remaining authorized streams and increment generation. Do not add newly attached displays automatically. If no authorized streams remain, mark capture unavailable without creating a new portal session.

- [ ] **Step 3: Remove portal-triggering media fallback paths**

Ensure `VideoSender::restart_capture`, bitrate fallback and stale/IDR recovery restart encoder/media state only. On authorization loss, stop video capture cleanly and preserve control/session diagnostics rather than invoking external portal capture.

- [ ] **Step 4: Update README**

Document the one-time KDE monitor selection, multi-monitor side-by-side behavior, persistent authorization, explicit reauthorization requirement after permission revocation, and the guarantee that normal recovery does not intentionally reopen the chooser.

- [ ] **Step 5: Run focused and existing Linux tests**

Run: `make test-linux-capture-recovery-contract test-multimonitor test-multimonitor-input test-input test-setup`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/linux_capture_session.cpp src/video_sender.cpp src/video_capture.cpp README tests/test_linux_capture_recovery_contract.cpp
git commit -m "fix: keep Linux capture authorization across recovery"
```

### Task 7: Full verification

**Files:**
- No production changes unless verification exposes a defect.

- [ ] **Step 1: Run the complete Linux suite**

Run: `make test && make test-hpi`
Expected: PASS.

- [ ] **Step 2: Run architecture/build contracts**

Run every new target plus the existing platform/build contracts. Expected: PASS with no portal creation remaining in encoder/media-restart code.

- [ ] **Step 3: Runtime verification on KDE Wayland**

On a Linux/KDE host, authorize two monitors once, connect a client, verify both monitors are equal-sized and side-by-side, exercise absolute pointer movement over both tiles, trigger bitrate/IDR/capture restart conditions, and verify the KDE source chooser does not reappear during the live session. Unplug one monitor and verify the remaining monitor continues without interactive portal fallback.

- [ ] **Step 4: Record verification truthfully**

If hardware runtime verification is not available in the execution environment, report source/unit/contract verification separately and leave KDE runtime behavior pending instead of claiming it passed.
