# OPAL Resilient Host, Input, and Video Design

Date: 2026-09-02
Status: Approved design; written spec ready for final review before implementation
Branch: `feature-resilient-host-input-video`

## Goal

Make OPAL behave like an always-available remote desktop rather than a terminal-bound one-shot session:

- the host runs continuously as a user-level background service;
- closing or crashing the client video player does not stop the host or tear down OPAL;
- mouse and keyboard input are reliable and release cleanly on disconnect;
- video targets 60 FPS and automatically recovers from capture/network/player stalls;
- the existing zrok2 tunnel, TLS identity/pairing, and connection-code model remain intact.

## Non-goals

- Replacing zrok2 with a new transport in this change.
- Replacing H.264/FLV with a new codec/container stack.
- Building a complete native video renderer in this change.
- Supporting multiple simultaneous controlling clients with independent key-state arbitration.
- Removing the advanced/manual OPAL commands.

A future UDP/QUIC/RTP-like media transport remains desirable because TCP head-of-line blocking can still hurt low-latency video under loss. This design makes the current tunnel robust without pretending TCP is the final media architecture.

## 1. Persistent host daemon

### Service ownership

`opal-host.service` becomes the owner of the complete host lifetime:

1. start OPAL's persistent zrok2 control/video share processes;
2. start the OPAL TLS control/video listeners;
3. wait for clients indefinitely;
4. return to waiting after each disconnect;
5. restart automatically if the host process itself crashes.

The service uses `Restart=always` with a short restart delay. Its `ExecStart` invokes a dedicated host-daemon path that starts the tunnels before `host_run()`; the service must not call a path that starts only the local listeners.

### Setup behavior

After first-time host setup succeeds, OPAL enables and starts the user service automatically. Hosting is no longer dependent on keeping the setup terminal open.

For an already-configured host, bare `opal` ensures the user service is enabled/running and reports host status instead of launching a second foreground listener that can conflict with the service.

Advanced foreground hosting remains available through an explicit debug/manual command.

### Disconnect semantics

A client disconnect, FFplay exit, capture restart, or video failure never terminates `opal-host.service`. Only an explicit host disable/clean or user-service shutdown stops the daemon.

## 2. Session model and reconnectable video

### Control session owns authorization

The authenticated control TLS connection is the authoritative session lifetime. Each control connection receives a random session token. The host stores that token while the control connection is alive and deletes it when the control connection closes.

Unlike the current one-shot behavior, a successful video authorization does not consume/delete the token. The same token may authenticate replacement video connections while its control session remains alive.

This permits video reconnection without re-pairing and without reconnecting keyboard/mouse control.

### Video startup handshake

The host must not report a working video session merely because TLS authorization succeeded.

New flow:

1. client opens video TLS and sends `VIDEO <session-token>`;
2. host validates that the associated control session is still alive;
3. host starts the capture child;
4. host waits for and buffers the first actual encoded media bytes;
5. host sends `READY` only after media exists;
6. host sends the buffered FLV bytes and continues the binary stream;
7. client starts/writes the player and prints `Connected` only after at least one real media chunk has been received and accepted by the player pipe;
8. if capture fails before first bytes, host sends an actionable `ERROR ...` and closes the video connection.

### Video recovery

Video is treated as disposable while control remains persistent.

A video session is considered stalled when one of these occurs:

- capture produces no bytes for roughly 3 seconds after it had started producing data;
- the host cannot deliver video bytes for roughly 3 seconds because the TLS/socket path is blocked;
- the video TLS connection dies;
- FFplay exits or its stdin pipe breaks unexpectedly.

For capture/network stalls, the client automatically reconnects the video TLS connection using the existing live control-session token. The host starts a fresh capture process and the player is restarted. This resets stale TCP/decoder state and resumes from a fresh stream/keyframe instead of leaving a frozen window indefinitely.

If the user closes the FFplay window, the OPAL client process does not exit. The player is treated as a replaceable child and is reopened while the OPAL control session remains active. The supported explicit release/exit mechanism remains `Ctrl+Alt+Shift+Q` (or terminating `opal` itself).

## 3. Capture process supervision

### Replace `popen` capture with an owned child

The host video worker replaces blocking `popen()/fread()` with an explicitly spawned child process whose stdout is connected to a pipe.

The host owns:

- capture PID;
- stdout pipe FD;
- process termination/reaping;
- startup timeout;
- media-byte liveness timestamps;
- termination on client disconnect or stalled network write.

`poll()`/equivalent is used so OPAL can detect no-data periods instead of blocking forever in `fread()`.

### GSR low-latency settings

For the existing GSR H.264 path, OPAL uses:

- `-f 60` from config (default 60);
- `-fm cfr` so a static/low-damage desktop still produces a continuous timed stream;
- `-keyint 1` so reconnection gets a fresh keyframe quickly;
- `-bm cbr` with configured kbps;
- `-k h264`;
- `-fallback-cpu-encoding yes` for Asahi/no-hardware-encoder systems;
- `-w portal` on Wayland and `-w screen` on X11;
- `-c flv` and no `-o`, which makes GSR write to stdout;
- normal-mode stderr suppression, with raw diagnostics under `OPAL_DEBUG=1`.

### Portal session restoration

Wayland capture uses GSR portal restoration support and stores its portal session token in OPAL-owned state under `~/.opal`. After the user approves the display once, capture restarts should request restoration of that approved portal session where the compositor/portal permits it, reducing repeated screen-selection prompts.

If restoration is rejected, OPAL falls back to the normal portal prompt rather than failing the host daemon.

### Network backpressure

The video TLS path receives a bounded write timeout. A client/network path that cannot accept media for the stall window is abandoned instead of allowing unlimited stale-video backlog to build. The capture child is stopped and the video connection is recreated while control remains alive.

This does not eliminate TCP head-of-line blocking, but it prevents a bad TCP period from leaving OPAL permanently seconds/minutes behind the live desktop.

## 4. Input architecture

### Raw relative client input

The client replaces the pointer-center/warp loop with XInput2 raw input when XI2 is available:

- raw relative motion for mouse movement;
- raw key press/release;
- raw button press/release;
- wheel events;
- no continuous pointer recentering/warping.

The existing X11 grab path remains only as a compatibility fallback when XI2 is unavailable.

The build links libXi and installation documentation/CI dependencies are updated accordingly.

### Keyboard semantics

OPAL continues to transport physical Linux-style key codes for remote-control/game compatibility. X11/XI2 keycodes are normalized through one tested mapping helper instead of scattering `keycode - 8` logic inline.

The protocol supports the Linux input key range through `KEY_MAX`, not only values below 256.

### Host uinput helper

`opal-input` enables key bits through `KEY_MAX` and keeps explicit sets of currently-held keys/buttons.

On EOF/termination it emits releases for every held key/button before destroying the uinput device, preventing stuck Shift/Ctrl/Alt/mouse buttons.

The host-side input writer checks write/flush failure. If `opal-input` dies, OPAL closes/reaps it, starts a new helper, and retries the input event once instead of silently losing input forever.

### Session cleanup

Each control session tracks the keys/buttons it has pressed. When that control TLS connection closes, OPAL sends releases for that session's still-held inputs. This is an additional safety layer on top of helper EOF cleanup.

## 5. Client lifecycle

The OPAL client becomes a supervisor for two logical channels:

- persistent control/input channel;
- restartable video/player channel.

The control connection remains alive while video is restarted. Input capture is active only while a viewer is active and OPAL owns remote-control focus. Explicit release clears local grabs/raw selections, releases all remote held input, closes control/video cleanly, and exits.

The player child is not the OPAL process lifetime. An FFplay crash/window close is recoverable and does not terminate the host or the OPAL control session.

## 6. Error handling and user-facing output

Normal mode remains concise. Expected user-facing states are similar to:

```text
Connecting to desktop through OPAL tunnel...
Connected. Ctrl+Alt+Shift+Q releases remote control.
Video stalled; reconnecting...
Video restored.
```

Raw GSR/zrok/FFplay/SDK diagnostics remain hidden unless `OPAL_DEBUG=1`.

Actionable permanent failures, such as missing `/dev/uinput`, no usable local input-capture path, repeated capture startup failure, or authentication failure, are emitted as OPAL-owned error messages.

The host daemon logs useful errors to the user journal, but a single session failure returns the daemon to its waiting state rather than exiting.

## 7. Files/components expected to change

- `src/main.cpp` — explicit daemon/foreground host command routing.
- `src/setup.cpp` — host setup automatically enables service; bare-host behavior.
- `src/system.cpp` — service lifecycle helpers/status.
- `systemd/opal-host.service` — full daemon ownership and `Restart=always`.
- `src/host.cpp` — persistent session table, reconnectable video auth, capture child supervision, input state cleanup.
- `src/client.cpp` — XI2 raw input, player supervision, video reconnect loop.
- `src/input_helper.cpp` — `KEY_MAX`, held-state cleanup.
- `src/media.cpp` / `include/opal/media.hpp` — GSR CFR/keyframe/portal restoration command generation and capture helpers as appropriate.
- `src/net.cpp` / header — bounded TLS write/read readiness helpers if needed.
- `Makefile` and CI — libXi build dependency and new tests.
- tests — focused unit/integration regressions described below.
- README/docs — service behavior, dependencies, debug instructions.

Implementation may split host/client supervision logic into small new source files if that keeps responsibilities isolated; unrelated refactoring is out of scope.

## 8. TDD and verification requirements

Tests are added before production behavior changes and must demonstrate RED before the corresponding fix.

Required regressions:

1. daemon command starts zrok host tunnels before local host listeners;
2. systemd service uses `Restart=always` and the complete daemon path;
3. first host setup enables/starts the host service;
4. sequential client/video sessions leave the host waiting for another connection;
5. video authorization token remains valid for replacement video connections while control is alive and is invalidated when control closes;
6. client does not report visual connection before first media bytes/`READY`;
7. GSR command uses 60 FPS config, CFR, one-second keyframes, CPU fallback, correct stdout mode, and portal restore options;
8. zero-byte capture startup becomes a clear error;
9. frozen capture is detected and the capture child is terminated/reaped;
10. blocked/stalled video delivery is bounded rather than building unlimited stale backlog;
11. player exit does not terminate OPAL control/session lifetime;
12. video reconnect succeeds without re-pairing;
13. XI2 raw mouse deltas are converted to OPAL relative mouse messages without pointer warping;
14. keyboard mapping is centralized/tested;
15. uinput accepts the Linux range through `KEY_MAX`;
16. all held keys/buttons are released on disconnect/helper EOF;
17. a dead input helper is restarted and the failed event is retried once;
18. normal mode does not leak child-process debug output; `OPAL_DEBUG=1` still does;
19. existing pairing, zrok connection-code, `opal clean`, install, smoke, and integration tests remain green.

Before merge, run the complete suite on the feature branch, inspect the diff for scope, fast-forward the exact tested revision to `main`, then independently verify `main` CI.

## 9. Success criteria

The change is complete when:

- the host can be logged in and left unattended with `opal-host.service` continuously waiting;
- closing/restarting the client's video window does not kill the host daemon or require a new connection code/pairing;
- mouse movement/buttons and keyboard presses/releases behave consistently without stuck inputs;
- the stream visibly continues to update at the configured 60 FPS target during normal desktop activity;
- a capture/network/player stall recovers automatically instead of leaving a permanently frozen stream;
- repeated connect/disconnect cycles do not leave stale GSR, FFplay, OPAL input, or zrok processes;
- normal terminal output remains concise.
