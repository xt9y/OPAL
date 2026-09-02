# OPAL Session Supervisor Stability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stabilize first-use tunnel propagation, persistent authenticated control, clean video generations, host zrok supervision, and sane mouse normalization while preserving the current zrok2 + TLS + GSR + FFplay architecture.

**Architecture:** Introduce `opal::SessionSupervisor` as the client owner of tunnel readiness, authenticated control generations, heartbeat/re-authentication, and video/player generations. Keep process/network primitives in their existing modules. Add small host-tunnel health primitives so `host_daemon()` can continuously repair dead zrok children without changing saved share tokens.

**Tech Stack:** C++20, OpenSSL TLS 1.3, X11/XInput2, zrok2 private shares/access, GPU Screen Recorder/FFmpeg, FFplay, POSIX process/socket APIs, Make, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-09-02-session-supervisor-stability-design.md`

## Global Constraints

- Tunnel/control establishment and recovery window: 30 seconds.
- Control heartbeat: PING every 2 seconds, PONG/read deadline 5 seconds.
- Capture startup deadline: 10 seconds.
- Live capture stall deadline: 3 seconds.
- TLS video write/backpressure allowance: 5 seconds.
- Default bitrate: 12,000 kbps.
- Video: H.264, 60 FPS CFR, MKV, one-second keyframes, explicit `-cursor yes`.
- Mouse normalization reference: 1000 DPI ~= 39,370 counts/m.
- Accept physical XI2 resolutions only in [5,000, 400,000] counts/m.
- Clamp automatic DPI scale to [0.25, 4.0].
- Clamp user mouse sensitivity to [0.1, 4.0], default 1.0.
- Paired recovery must use saved AUTH and must never silently fall back to PAIR.
- A video generation is always a new capture + new TLS connection + new player process; bytes from different generations must never share player stdin.

---

### Task 1: Lock down deterministic media, mouse, and connection-code policy

**Files:**
- Modify: `tests/test_input.cpp`
- Modify: `tests/test_media.cpp`
- Modify: `tests/test_setup.cpp`
- Modify: `include/opal/input.hpp`
- Modify: `src/input.cpp`
- Modify: `src/media.cpp`
- Modify: `src/host.cpp`
- Modify: `src/setup.cpp`

**Interfaces:**
- Produces: `double mouse_normalization_scale(int resolution)` and `double clamp_mouse_sensitivity(double sensitivity)` in `opal/input.hpp`.
- `normalized_motion_command(...)` gains an optional sensitivity argument defaulting to 1.0.

- [ ] **Step 1: Write failing tests**

Add assertions equivalent to:

```cpp
assert(opal::mouse_normalization_scale(1000)==1.0);
assert(opal::mouse_normalization_scale(0)==1.0);
assert(opal::mouse_normalization_scale(125984)>0.25 && opal::mouse_normalization_scale(125984)<0.33);
assert(opal::mouse_normalization_scale(5000)==4.0);
assert(opal::mouse_normalization_scale(400000)==0.25);
assert(opal::clamp_mouse_sensitivity(0.01)==0.1);
assert(opal::clamp_mouse_sensitivity(9.0)==4.0);
assert(opal::normalized_motion_command(10,0,1000,1000)=="MOUSE 10 0");
```

In media tests assert GSR command includes `-cursor yes`, `-c mkv`, and no `-c flv`; FFmpeg fallback ends in Matroska. Assert source/config text changes the default bitrate to 12000. In setup tests assert the malformed-code error contains `Expected: opal:CONTROL,VIDEO`.

- [ ] **Step 2: Run CI and verify RED**

Expected: compile/test failure because mouse helper functions do not exist and media/error/default expectations are not yet met.

- [ ] **Step 3: Implement minimal policy changes**

Implement:

```cpp
double mouse_normalization_scale(int resolution) {
    if(resolution < 5000 || resolution > 400000) return 1.0;
    constexpr double target = 1000.0 / 0.0254;
    return std::clamp(target / static_cast<double>(resolution), 0.25, 4.0);
}

double clamp_mouse_sensitivity(double sensitivity) {
    return std::clamp(sensitivity, 0.1, 4.0);
}
```

Apply automatic scale first, then sensitivity. Change GSR to `-cursor yes ... -c mkv`; change FFmpeg to `-f matroska pipe:1`; set `host_setup()` default bitrate and `video_client()` fallback bitrate to 12000; improve setup malformed-code text.

- [ ] **Step 4: Run CI and verify GREEN**

Expected: all current tests plus new policy assertions pass.

- [ ] **Step 5: Commit**

Commit message: `fix: stabilize OPAL media and mouse defaults`.

---

### Task 2: Make tunnel access retry propagation and host shares repairable after startup

**Files:**
- Modify: `tests/test_tunnel.cpp`
- Modify: `tests/test_tunnel_recovery.cpp`
- Modify: `include/opal/tunnel.hpp`
- Modify: `src/tunnel.cpp`

**Interfaces:**
- Produces: `bool tunnel_host_healthy()` and `int tunnel_host_ensure_running()`.
- `tunnel_access(...)` retains its public signature but internally retries access-child establishment for up to 30 seconds.

- [ ] **Step 1: Write failing tests**

Extend fake zrok with a mode where the first `access private` invocation for each token exits with an `accessNotFound`-style failure and later invocations bind both local ports. Assert one call to `tunnel_access()` succeeds and the log shows multiple access launches.

Extend stale-share recovery with a mode where a successfully started share child later exits. After killing/allowing one child to die, assert `tunnel_host_healthy()` is false; call `tunnel_host_ensure_running()`; assert health returns true and saved `opal-ctl-preserved` / `opal-vid-preserved` tokens remain the ones used/repaired.

- [ ] **Step 2: Run CI and verify RED**

Expected: tunnel propagation test fails because `tunnel_access()` is one-shot; compile fails for missing host health functions.

- [ ] **Step 3: Implement bounded tunnel retry and health primitives**

Refactor access launch into an internal attempt helper. `tunnel_access()` should:

```cpp
auto deadline = steady_clock::now() + 30s;
do {
    stop_recorded(access_pid_file);
    launch control + video access children;
    if (wait_for_access_endpoints_for_this_attempt(...)) record and return true;
    terminate both children;
    sleep a short retry interval;
} while (steady_clock::now() < deadline);
return false;
```

Keep each individual access attempt short enough that failed propagation can be retried inside the 30-second window.

Implement `tunnel_host_healthy()` by validating both recorded PID/start-time pairs are alive and are OPAL zrok private-share children. Implement `tunnel_host_ensure_running()` as `healthy ? 0 : tunnel_host_start()` so the existing exact-token repair path is reused.

- [ ] **Step 4: Run CI and verify GREEN**

Expected: delayed/first-failure access recovers and host tunnel health tests pass.

- [ ] **Step 5: Commit**

Commit message: `fix: supervise OPAL zrok tunnel health`.

---

### Task 3: Add timeout-safe control reads required by heartbeat

**Files:**
- Modify: `tests/test_net.cpp`
- Modify: `include/opal/net.hpp`
- Modify: `src/net.cpp`

**Interfaces:**
- Produces: `bool tls_read_line_timeout(SSL *ssl, std::string &line, int timeout_ms, size_t limit=8192)`.

- [ ] **Step 1: Write failing test**

Create a TLS server that delays a line for >1 second but <5 seconds and assert `tls_read_line_timeout(..., 5000)` succeeds. Create a silent peer and assert a 200 ms read returns false within a bounded time.

- [ ] **Step 2: Run CI and verify RED**

Expected: compile failure because `tls_read_line_timeout` does not exist.

- [ ] **Step 3: Implement minimal nonblocking SSL line read**

Use the existing `wait_for_ssl_fd()` pattern: temporarily set the SSL fd nonblocking, loop `SSL_read`, accept WANT_READ/WANT_WRITE until one absolute deadline, restore flags on every exit, and enforce the line length limit.

- [ ] **Step 4: Run CI and verify GREEN**

Expected: net retry/write timeout tests and new read-timeout tests pass.

- [ ] **Step 5: Commit**

Commit message: `feat: add bounded TLS control reads`.

---

### Task 4: Introduce the logical SessionSupervisor and clean video generations

**Files:**
- Create: `include/opal/session.hpp`
- Create: `src/session.cpp`
- Create: `tests/test_session.cpp`
- Modify: `Makefile`
- Modify: `src/client.cpp`

**Interfaces:**
- Produces class `opal::SessionSupervisor` with a small client-facing surface:

```cpp
struct SessionOptions {
    std::string target;
    int control_port = 47990;
    int video_port = 47991;
    bool tunneled = false;
    std::string control_token;
    std::string video_token;
    std::string fingerprint;
    std::string client_public_key;
    std::string client_private_key_path;
    bool paired = false;
    std::string pairing_password;
};

class SessionSupervisor {
public:
    explicit SessionSupervisor(SessionOptions options);
    ~SessionSupervisor();
    bool start();
    void stop();
    bool send_input(const std::string &command);
    unsigned long control_generation() const;
    bool media_started() const;
    std::string last_error() const;
};
```

The exact private decomposition may vary, but all control SSL reads/writes must be serialized and all control recovery must issue AUTH when `paired == true`.

- [ ] **Step 1: Write failing session tests**

Use local TLS test servers and `OPAL_PLAYER_CMD` / capture overrides rather than mock-only assertions. Cover:

```text
first authentication can PAIR once
subsequent control generation uses AUTH
silent/missing PONG marks generation dead within 5 seconds
control reconnect increases control_generation and gets a fresh video token
player exit creates a fresh video TLS/player generation
no player process receives bytes from two video generations
```

Make the fake player write a generation marker/count and exit after bounded input so restart is observable.

- [ ] **Step 2: Run CI and verify RED**

Expected: compile failure because `opal/session.hpp` and SessionSupervisor do not exist.

- [ ] **Step 3: Implement minimal SessionSupervisor**

Responsibilities:

```text
start:
  ensure tunnel access if tunneled
  connect control TLS inside 30 s
  verify pinned fingerprint
  CHALLENGE -> PAIR once or AUTH if already paired
  save current video token
  start heartbeat thread
  start video-generation thread

heartbeat:
  every 2 s serialize PING/PONG on control SSL
  use 5 s tls_read_line_timeout
  on failure invoke one serialized recover_control()

recover_control:
  interrupt current video generation
  close old control
  re-run tunnel_access when tunneled
  reconnect within 30 s
  AUTH only with saved identity
  obtain fresh video token
  increment generation
  allow video thread to start a clean generation

video generation:
  connect video TLS using current token
  start one new player process
  stream only this TLS connection to this player's stdin
  on player/TLS failure close both before retrying
  report Video connecting/connected/interrupted/restored messages
```

Move the existing player/video helpers out of `client.cpp` into `session.cpp`. `client.cpp` should retain X11/XInput2 capture and call `send_input()` instead of writing directly to one SSL pointer. When `control_generation()` changes, clear the local `HeldInputState` so old held-state assumptions are not carried into the new control generation.

- [ ] **Step 4: Run CI and verify GREEN**

Expected: new session tests pass and the full existing suite remains green.

- [ ] **Step 5: Commit**

Commit message: `feat: supervise OPAL client sessions`.

---

### Task 5: Tie host daemon and integration behavior to the supervisor

**Files:**
- Modify: `src/host.cpp`
- Modify: `tests/integration.sh`
- Modify: `tests/test_daemon.cpp` if useful for source-level daemon assertions

**Interfaces:**
- `host_daemon()` keeps `host_run()` and one tunnel-health worker alive together.

- [ ] **Step 1: Write failing integration regression**

Extend integration fixture so it can kill/restart one logical layer without ending the client process. Verify:

```text
initial run prompts/uses pairing exactly once
player exits -> player count increases and Video restored appears
control connection is deliberately dropped -> client reconnects with AUTH and no pairing prompt
new control generation causes a new video generation
host remains alive
```

Where direct socket targeting is awkward, add a test-only environment hook such as `OPAL_TEST_CONTROL_CLOSE_AFTER_PINGS=1` on the host; keep it isolated to test behavior and default it off.

- [ ] **Step 2: Run CI and verify RED**

Expected: integration client exits or fails to create a fresh authenticated control/video generation.

- [ ] **Step 3: Add host continuous tunnel worker and test hook**

In `host_daemon()` start a worker that checks `tunnel_host_healthy()` periodically and calls `tunnel_host_ensure_running()` when required. Stop/join it when host_run exits. Implement only the minimum test hook required to deterministically close one authenticated control generation.

Change host video writes from 1000 ms to 5000 ms for both first media bytes and live media writes.

- [ ] **Step 4: Run full CI and verify GREEN**

Expected: clean build, unit tests, clean/install tests, smoke test, and extended integration test all pass.

- [ ] **Step 5: Commit**

Commit message: `fix: recover complete OPAL sessions`.

---

### Task 6: Final verification and merge

**Files:**
- Review all changes against the spec.

- [ ] **Step 1: Verify branch CI at the exact head SHA**

Required result: GitHub Actions `CI` conclusion `success` for the final implementation head.

- [ ] **Step 2: Review diff for spec coverage**

Confirm: 30 s tunnel/recovery, 2 s/5 s heartbeat, 10 s startup, 3 s capture stall, 5 s media write, 12 Mbps default, MKV, cursor yes, clean video generations, AUTH-only paired recovery, mouse plausibility/clamps, improved invalid-code message, continuous host tunnel repair.

- [ ] **Step 3: Merge into main**

Fast-forward `main` to the verified branch head when possible so the verified SHA is exactly what ships.

- [ ] **Step 4: Verify main CI at the merged SHA**

Required result: GitHub Actions `CI` conclusion `success` on `main` for the same implementation revision.
