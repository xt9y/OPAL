# OPAL Resilient Host, Input, and Video Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make OPAL an always-available host service with reliable keyboard/mouse injection and a 60 FPS video session that detects and recovers from player/capture stalls.

**Architecture:** Keep the existing zrok2 + TLS + H.264/FLV design, but separate process lifetime from session lifetime. systemd owns a dedicated OPAL daemon path; control TLS sessions remain persistent while video capture/player sessions are replaceable; input is normalized into Linux key/button semantics and injected through a supervised uinput helper.

**Tech Stack:** C++20, OpenSSL, X11/XInput2, Linux uinput, gpu-screen-recorder, FFplay, zrok2, systemd user services.

**Spec:** `docs/superpowers/specs/2026-09-02-resilient-host-input-video-design.md`

## Global Constraints

- Keep zrok2 as the required normal transport; do not add direct/LAN fallback to normal `opal`.
- Keep TLS identity/pairing and existing OPAL connection codes compatible.
- Default video target remains 60 FPS, H.264, FLV, 20000 kbps.
- Preserve `OPAL_DEBUG=1` as the raw subprocess-diagnostics opt-in; normal mode stays concise.
- `opal clean` must still preserve zrok2 login/environment state.
- A client/player/video failure must never stop the persistent host daemon.
- Explicit remote-control release remains `Ctrl+Alt+Shift+Q`.

---

## File structure

- `src/main.cpp` / `include/opal/host.hpp`: add explicit `host daemon` routing so systemd owns tunnel + listener lifetime while `opal host` remains a manual foreground/debug path.
- `src/setup.cpp`, `src/system.cpp`, `include/opal/system.hpp`, `systemd/opal-host.service`: automatically enable/start the host service after host setup and make bare `opal` on an existing host ensure the daemon is running instead of binding a second foreground listener.
- `src/media.cpp`, `include/opal/media.hpp`: generate low-latency CFR GSR commands and expose a supervised capture-child abstraction.
- `src/host.cpp`: keep authenticated control sessions alive, allow repeat video authorization, supervise capture children, enforce first-media readiness and stall timeouts, and recover/reap input helpers.
- `src/client.cpp`: supervise FFplay/video reconnect independently of the control connection and use XInput2 raw input with a compatibility fallback.
- `src/input_helper.cpp`, new `include/opal/input.hpp`, new `src/input.cpp`: centralize X11-to-Linux keycode normalization, held-input tracking helpers, and full Linux `KEY_MAX` uinput support.
- `src/net.cpp`, `include/opal/net.hpp`: add bounded TLS write support for media backpressure.
- `Makefile`, `.github/workflows/ci.yml`: link/install libXi and add focused tests.
- `tests/test_daemon.cpp`, `tests/test_media.cpp`, `tests/test_input.cpp`, `tests/integration.sh`, `tests/smoke.sh`: regressions for daemon ownership, video readiness/recovery, raw input semantics, cleanup, and repeated sessions.

---

### Task 1: Persistent host daemon ownership

**Files:**
- Modify: `include/opal/host.hpp`
- Modify: `src/main.cpp`
- Modify: `src/setup.cpp`
- Modify: `src/system.cpp`
- Modify: `include/opal/system.hpp`
- Modify: `systemd/opal-host.service`
- Create: `tests/test_daemon.cpp`
- Modify: `tests/test_setup.cpp`
- Modify: `Makefile`

**Interfaces:**
- Produces: `int opal::host_daemon();`
- Produces: `bool opal::host_service_active();`
- `host_daemon()` calls `tunnel_host_start()` exactly once before `host_run()` and returns its status.

- [ ] **Step 1: Write failing daemon/service tests**

Create `tests/test_daemon.cpp` that reads the installed/source unit and verifies the dedicated daemon command plus restart policy:

```cpp
#include <cassert>
#include <fstream>
#include <sstream>
#include <string>
int main(){
    std::ifstream f("systemd/opal-host.service");
    std::stringstream ss; ss<<f.rdbuf(); auto s=ss.str();
    assert(s.find("ExecStart=/usr/local/bin/opal host daemon")!=std::string::npos);
    assert(s.find("Restart=always")!=std::string::npos);
}
```

Update `tests/test_setup.cpp` so first host setup expects `host_service_calls==1`, `tunnel_start_calls==0`, and `host_run_calls==0`; configured-host bare `opal` expects service activation rather than foreground hosting.

- [ ] **Step 2: Run tests and verify RED**

Run: `make test-daemon test-setup`

Expected: daemon test fails on the old `ExecStart=/usr/local/bin/opal host`; setup test fails because setup still prompts/starts foreground hosting.

- [ ] **Step 3: Implement daemon routing and service semantics**

Add to `include/opal/host.hpp`:

```cpp
namespace opal { int host_setup(); int host_run(); int host_daemon(); }
```

Add production behavior equivalent to:

```cpp
int host_daemon(){
    if(tunnel_host_start()!=0) return 1;
    return host_run();
}
```

Route `opal host daemon` to `host_daemon()`. Keep plain `opal host` mapped to `host_run()` for the existing manual/local integration path. Change the systemd unit to `ExecStart=/usr/local/bin/opal host daemon` and `Restart=always`.

Change first-time host setup to enable/start the service automatically after tunnel setup and return to the shell. For an existing `role=host`, bare `opal` calls `host_service(true)` and prints a concise host-service status instead of running listeners in the terminal.

- [ ] **Step 4: Run tests and verify GREEN**

Run: `make test-daemon test-setup`

Expected: both pass.

- [ ] **Step 5: Commit**

Commit message: `feat: make OPAL host a persistent daemon`

---

### Task 2: Supervised capture and first-media readiness

**Files:**
- Modify: `include/opal/media.hpp`
- Modify: `src/media.cpp`
- Modify: `src/host.cpp`
- Create: `tests/test_media.cpp`
- Modify: `tests/test_core.cpp`
- Modify: `Makefile`

**Interfaces:**
- Produces: `struct CaptureProcess { pid_t pid; int fd; };`
- Produces: `CaptureProcess start_capture(const std::string &command);`
- Produces: `void stop_capture(CaptureProcess &capture);`
- Produces: `int read_capture(CaptureProcess &capture, void *buffer, size_t size, int timeout_ms);` returning bytes, `0` on EOF, `-2` on timeout, `-1` on error.

- [ ] **Step 1: Write failing media-command and capture timeout tests**

Require the GSR command to contain:

```text
-f 60
-fm cfr
-keyint 1
-k h264
-fallback-cpu-encoding yes
-bm cbr
-c flv
```

and still contain no `-o -`.

In `tests/test_media.cpp`, use `OPAL_CAPTURE_CMD`-style shell commands such as `printf MEDIA` and `sleep 2` to verify first bytes and a bounded timeout from `read_capture()`.

- [ ] **Step 2: Run tests and verify RED**

Run: `make test-core test-media`

Expected: command test fails because CFR/keyframe flags are absent; capture API fails to compile because it does not exist.

- [ ] **Step 3: Implement owned capture child**

Replace `popen` ownership with `pipe` + `fork` + `/bin/sh -c <command>` so stdout is an OPAL-owned nonblocking/readable FD and the child PID can be terminated/reaped. `stop_capture()` sends SIGTERM, waits briefly, SIGKILLs if required, closes the FD, and `waitpid()`s.

Generate GSR command with `-fm cfr -keyint 1`. Keep normal stderr redirected to `/dev/null`; leave it attached under `OPAL_DEBUG=1`.

- [ ] **Step 4: Make video authorization wait for media**

In `host.cpp`, after `VIDEO <token>` authorization, start capture first. Wait up to about 10 seconds for the first encoded bytes. If none arrive, send `ERROR capture-startup` and close only that video session. If bytes arrive, send `READY`, then send the already-read bytes followed by the rest of the stream.

Do not delete the session token when authorizing video; invalidate it only when the associated control connection ends.

- [ ] **Step 5: Run tests and verify GREEN**

Run: `make test-core test-media`

Expected: pass.

- [ ] **Step 6: Commit**

Commit message: `feat: supervise OPAL capture sessions`

---

### Task 3: Bounded media writes and reconnectable player sessions

**Files:**
- Modify: `include/opal/net.hpp`
- Modify: `src/net.cpp`
- Modify: `src/client.cpp`
- Modify: `src/host.cpp`
- Modify: `tests/test_net.cpp`
- Modify: `tests/integration.sh`

**Interfaces:**
- Produces: `bool tls_write_all_timeout(SSL *ssl,const void *data,size_t size,int timeout_ms);`
- Client video supervisor keeps the authenticated control `SSL*` and session token alive while it reconnects video TLS and FFplay.

- [ ] **Step 1: Write failing bounded-write/reconnect regressions**

Add a net test that creates a peer which stops reading and verifies `tls_write_all_timeout(..., 500)` returns false within a bounded interval.

Extend integration with a fake player that exits after consuming the first media stream and records each launch. Keep the control connection alive long enough to assert OPAL launches a replacement video/player session without re-pairing.

- [ ] **Step 2: Run tests and verify RED**

Run: `make test-net && BIN=$(pwd)/build/opal INPUT_BIN=$(pwd)/build/opal-input ./tests/integration.sh`

Expected: bounded-write API missing and player-exit path terminates/blocks current session behavior.

- [ ] **Step 3: Implement bounded TLS writes**

Use the TLS socket FD with `poll(POLLOUT)` and a monotonic deadline. Retry `SSL_write` on WANT_READ/WANT_WRITE until completion or deadline; return false on timeout/fatal error.

Use this bounded path for video media writes so a blocked client cannot accumulate unlimited stale stream latency.

- [ ] **Step 4: Implement client video supervisor**

Factor video setup into a function that:

```text
connect video TLS -> VIDEO token -> wait READY -> read first media chunk -> start/write FFplay -> continue
```

Only after READY plus the first received/written media chunk should normal output print `Connected`.

If FFplay exits, the video TLS dies, or video bytes stop arriving for the stall interval, close only the video/player resources, print `Video stalled; reconnecting...`, reconnect with the same live control token, then print `Video restored.` after fresh media is flowing.

`Ctrl+Alt+Shift+Q` remains the explicit full client-session exit.

- [ ] **Step 5: Run tests and verify GREEN**

Run: `make test-net && BIN=$(pwd)/build/opal INPUT_BIN=$(pwd)/build/opal-input ./tests/integration.sh`

Expected: pass, including repeated player launch without a new pair operation.

- [ ] **Step 6: Commit**

Commit message: `feat: recover stalled OPAL video sessions`

---

### Task 4: Reliable Linux input semantics and helper recovery

**Files:**
- Create: `include/opal/input.hpp`
- Create: `src/input.cpp`
- Modify: `src/input_helper.cpp`
- Modify: `src/host.cpp`
- Create: `tests/test_input.cpp`
- Modify: `Makefile`

**Interfaces:**
- Produces: `int opal::linux_keycode_from_x11(unsigned int keycode);`
- Produces: `class HeldInputState` with `press_key`, `release_key`, `press_button`, `release_button`, `release_commands()`.

- [ ] **Step 1: Write failing mapping/state tests**

Require `linux_keycode_from_x11(9)==1`, `linux_keycode_from_x11(108)==100`, invalid values to return `0`, and held-state cleanup to emit releases only for inputs still down.

Add a source/behavior regression that `opal-input` uses `KEY_MAX` rather than a hard-coded 255/256 bound.

- [ ] **Step 2: Run tests and verify RED**

Run: `make test-input`

Expected: missing input helpers / hard-coded input range failure.

- [ ] **Step 3: Implement input utilities and full uinput range**

Centralize X11 keycode normalization (`keycode >= 8 ? keycode-8 : 0`) and clamp to `KEY_MAX`. In `opal-input`, register EV_KEY bits from 1 through `KEY_MAX` and maintain sets of down keys/buttons. On EOF, emit release events for all held inputs before `UI_DEV_DESTROY`.

- [ ] **Step 4: Supervise host input helper**

Replace fire-and-forget `FILE*` behavior with failure-aware helper ownership. If write/flush fails, close/reap the helper, respawn it, and retry the event once. Track each control session's held inputs; on control disconnect, send release commands for that session before deleting its session token.

- [ ] **Step 5: Run tests and verify GREEN**

Run: `make test-input`

Expected: pass.

- [ ] **Step 6: Commit**

Commit message: `fix: make OPAL input injection reliable`

---

### Task 5: XInput2 raw client mouse/keyboard capture

**Files:**
- Modify: `src/client.cpp`
- Modify: `Makefile`
- Modify: `.github/workflows/ci.yml`
- Modify: `tests/test_input.cpp`
- Modify: `tests/smoke.sh`

**Interfaces:**
- Uses: `linux_keycode_from_x11()` from Task 4.
- XI2 raw-event path emits existing text protocol commands: `KEY`, `MOUSE`, `BUTTON`, `WHEEL`.

- [ ] **Step 1: Write failing dependency/raw-input tests**

Add build/test assertions that libXi is linked and CI installs `libxi-dev`. Add pure input-event conversion tests where raw delta `(12,-7)` maps to `MOUSE 12 -7` without any pointer warp dependency.

- [ ] **Step 2: Run tests and verify RED**

Run: `make test-input`

Expected: XI2/input conversion support missing.

- [ ] **Step 3: Implement XI2 raw input path**

Open the X display, query XInput2, select raw key/button/motion events on the root window, and translate `XIRawEvent` values into OPAL protocol messages. Do not warp the pointer in the XI2 path. Keep the existing grab/warp loop as a compatibility fallback only when XI2 is unavailable.

Track locally held keys/buttons so the explicit release path sends remote releases before closing TLS.

- [ ] **Step 4: Update build dependencies**

Add `-lXi` to `LDLIBS`; add `libxi-dev` to GitHub Actions dependencies. `opal doctor` should report XInput2 client capability distinctly from the fallback X11 display check.

- [ ] **Step 5: Run tests and verify GREEN**

Run: `make test-input test && make clean && make`

Expected: all pass/build cleanly apart from pre-existing compiler warnings.

- [ ] **Step 6: Commit**

Commit message: `feat: use raw relative input for OPAL control`

---

### Task 6: Portal restoration, repeated sessions, docs, and final verification

**Files:**
- Modify: `src/media.cpp`
- Modify: `include/opal/media.hpp`
- Modify: `src/host.cpp`
- Modify: `tests/test_media.cpp`
- Modify: `tests/integration.sh`
- Modify: `README.md`

**Interfaces:**
- Portal restoration state lives under OPAL-owned `~/.opal` state and is passed to GSR only when a saved token exists.

- [ ] **Step 1: Write failing portal/repeated-session tests**

Require Wayland capture command generation to include the GSR portal restore-file/token argument when supplied and omit it when absent. Extend integration to complete two sequential client connections against one host process and verify the host remains alive between them.

- [ ] **Step 2: Run tests and verify RED**

Run: `make test-media && BIN=$(pwd)/build/opal INPUT_BIN=$(pwd)/build/opal-input ./tests/integration.sh`

Expected: portal restoration support/repeated-session behavior not yet fully covered.

- [ ] **Step 3: Implement portal restoration and session cleanup**

Store/reuse the GSR portal restore token/file inside `~/.opal`; if GSR/portal rejects restoration, retry capture without restoration so the user can select a screen again. Ensure every capture/player/input child is terminated/reaped on its session end and the host listener remains active.

- [ ] **Step 4: Update README**

Document: host auto-daemon behavior, `systemctl --user status opal-host.service`, `OPAL_DEBUG=1`, libXi dependency, client release shortcut, and automatic video recovery. Preserve the existing documentation link and zrok2 setup instructions.

- [ ] **Step 5: Run complete verification**

Run: `make clean && make test`

Then inspect `git diff main...feature-resilient-host-input-video` for scope. Required result: only spec/plan plus daemon/input/media/network/build/tests/docs changes from this design.

- [ ] **Step 6: Commit**

Commit message: `feat: complete resilient OPAL remote sessions`

- [ ] **Step 7: Merge and verify main**

Fast-forward the exact green feature revision into `main`. Run/observe independent `main` CI and require build + complete test suite success before reporting completion.
