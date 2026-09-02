# OPAL Session Supervisor Stabilization Design

Date: 2026-09-02
Status: approved design, pending implementation

## Goal

Make normal `opal` use stable enough that first-time pairing, reconnects, video, keyboard, and mouse behave as one resilient remote-desktop session rather than several loosely coupled subprocesses and sockets.

The user-facing goal is simple:

```text
opal
  -> establish tunnel
  -> pair once if needed
  -> show remote desktop
  -> keyboard + mouse remain usable
  -> recover transient tunnel/video failures automatically
```

The host remains a persistent background service. Closing the client video window or a temporary network interruption must not require `opal clean`, a new connection code, or a new pairing unless the host identity itself changed.

## Observed failures this design must eliminate

The stabilization pass is driven by concrete runtime failures already observed:

1. First valid client setup can save the host, then fail before password prompt because zrok access/share propagation is not yet ready.
2. A subsequent run succeeds and asks for the pairing password, proving setup/auth is racing network readiness rather than being fundamentally invalid.
3. The live client reports H.264/FLV corruption such as invalid NAL sizes, packet corruption, and track-size mismatch after recovery/reconnect activity.
4. The client display can stop updating even while the host desktop keeps changing.
5. Keyboard can work briefly and then stop when the control path dies.
6. Mouse input can be invisible or massively over-scaled because synthetic XI2/XWayland resolution values are treated as physical sensor resolution.
7. zrok share processes can disappear while OPAL state still contains valid-looking saved share tokens.
8. Normal mode can hide the precise layer that is reconnecting or failing, making the whole session appear randomly broken.

## Architecture

### Logical session supervisor

The client will introduce one logical session supervisor responsible for these independently recoverable layers:

```text
Saved host / connection code
        |
        v
Tunnel access supervisor
        |
        v
Authenticated control session
        |
        +--------------------+
        |                    |
        v                    v
Input capture           Video session
                             |
                             v
                         Player process
```

The control session is the authority for the logical remote-desktop session. Video and the local player are children of that authenticated session.

A failure in a child must not destroy unrelated healthy layers. A failure in the control session invalidates its video token, so the supervisor must rebuild control authentication first, then build a new video generation.

### State model

The client supervisor will have explicit states rather than implicit nested retry loops:

```text
TUNNEL_CONNECTING
CONTROL_CONNECTING
PAIRING
AUTHENTICATING
VIDEO_CONNECTING
RUNNING
RECOVERING_CONTROL
RECOVERING_VIDEO
STOPPING
```

State changes are user-visible only when useful. Normal output should remain compact.

## Tunnel establishment and first login

### First-time setup

After the user enters a valid `opal:CONTROL,VIDEO` connection code, OPAL saves the host name and then continues establishment instead of treating the first zrok failure as final.

Tunnel establishment gets a bounded propagation window of 30 seconds. During that window OPAL may restart its local `zrok2 access private` children and retry end-to-end TLS readiness.

Expected normal first-use behavior:

```text
Connecting to desktop through OPAL tunnel...
Pairing password: XXXX-XXXX
Connected. Ctrl+Alt+Shift+Q releases remote control.
```

The pairing password prompt must occur exactly once after the control host is reachable. A tunnel failure before control authentication must never incorrectly mark the host as paired.

### Already paired clients

Once pairing succeeds, reconnects use the saved client identity and `AUTH` challenge response. Temporary tunnel, control, video, or player failures must never ask for the pairing password again.

If AUTH is rejected because authorization was actually removed on the host, OPAL should say so explicitly and stop rather than silently falling back to PAIR.

## Host tunnel supervision

`opal-host.service` remains the persistent owner of the host lifetime.

The host daemon must continuously supervise its two persistent zrok share processes. If either exits:

1. stop the surviving sibling if necessary to avoid half-alive state;
2. recreate the missing persistent reservation using the same saved token;
3. restart both share children;
4. keep the OPAL TLS listeners and host identity unchanged.

This preserves saved client connection codes.

`opal restart` remains a manual recovery command, but healthy normal use must not require it.

## Control session stability

### Heartbeat

The logical session sends periodic control `PING` requests and expects `PONG` responses. A missed/failed heartbeat marks the control generation dead.

### Control recovery

When the control session fails:

1. stop sending input;
2. locally release any held client keys/buttons;
3. terminate the current video/player generation;
4. re-establish zrok access if necessary;
5. reconnect control TLS;
6. authenticate with saved `AUTH` identity;
7. obtain a new video session token;
8. start a fresh video generation;
9. resume input capture.

The session remains logically connected from the user's perspective unless recovery exceeds the bounded retry window.

### Input helper health

Host input injection keeps the existing `opal-input` uinput helper and automatic respawn behavior. Failed input writes remain recoverable. Held host-side keys/buttons are released when a control generation ends.

## Video framing and recovery

### Never concatenate generations

A video generation is defined as:

```text
new capture process
+ new video TLS connection
+ new player stdin/process
```

Encoded bytes from different generations must never be concatenated into one player stream.

Whenever the player exits, video TLS fails, the control generation changes, or the capture stalls, OPAL closes all three components of that generation and creates a completely new generation.

This requirement directly prevents reconnecting in the middle of an old container/H.264 stream.

### Container and capture settings

GPU Screen Recorder remains the preferred capture path.

The stabilized default target is:

- H.264;
- 60 FPS CFR;
- one-second keyframe interval;
- CPU encoder fallback when hardware H.264 is unavailable;
- cursor explicitly included;
- 12,000 kbps default video bitrate;
- Matroska streaming container for new GSR sessions;
- portal-session restoration retained on Wayland.

The FFmpeg fallback should use matching low-latency H.264 and Matroska output where practical.

Existing user configuration can override the bitrate. The default changes from 20,000 kbps to 12,000 kbps to reduce sensitivity to tunnel jitter while retaining good desktop quality.

### Network backpressure

The current one-second TLS media-write deadline is too aggressive for a tunneled remote desktop. The new video delivery loop must tolerate temporary backpressure without corrupting the stream.

The implementation should use a bounded but substantially more tolerant write/stall policy, with separate concepts for:

- short network backpressure;
- capture producing no bytes;
- definitively dead TLS socket.

A transient delay must not cut the encoder stream in the middle of a packet simply because one second elapsed.

### Recovery messages

Normal user-facing messages:

```text
Video connecting...
Video connected.
Video interrupted; recovering...
Video restored.
```

Repeated internal retry noise remains hidden unless `OPAL_DEBUG=1`.

## Keyboard input

Keyboard capture remains X11/XInput2-based for the current client architecture, with the OPAL-launched FFplay forced into the XWayland/X11 domain when an X display exists.

A successful `XGrabKeyboard()` uses core `KeyPress`/`KeyRelease` events as canonical input. XI raw key events remain fallback only when the grab fails, preventing duplicate key injection.

Input capture belongs to the logical session supervisor rather than one disposable control TLS object. During control recovery, input capture pauses, held inputs are released, and capture resumes against the new authenticated control connection.

## Mouse input

### Resolution normalization

The current normalization accepts any XI2 resolution >= 1000 counts/meter. Synthetic XWayland values can therefore create enormous multipliers.

The new normalization will:

1. only accept a reported resolution when it is within a plausible physical mouse range;
2. reject synthetic/implausible resolution metadata and fall back to neutral scaling;
3. clamp automatic scale factors to a safe range so metadata can never generate extreme acceleration;
4. retain a user-configurable sensitivity multiplier, default `1.0`;
5. apply sensitivity after physical normalization.

The exact constants must be unit-tested. A synthetic resolution around 1000 counts/meter must never create a ~39x multiplier.

### Cursor visibility

GSR capture must explicitly include the host cursor so successful remote pointer movement is visible in the stream.

## Failure handling

### Invalid connection code

Malformed input such as:

```text
opal,opal-ctl-...,opal-vid-...
```

should continue to be rejected, but the error should state the expected format:

```text
Invalid OPAL connection code. Expected: opal:CONTROL,VIDEO
```

### Exhausted recovery

If the 30-second establishment/recovery window expires, OPAL prints the failing layer, for example:

```text
Tunnel connection could not be established.
Control connection could not be restored.
Video could not be restored.
```

It must not print a generic successful `Connected` state while video has not reached the player.

## Files and boundaries

The implementation should introduce focused supervisor code instead of continuing to grow `client.cpp`.

Proposed boundary:

```text
include/opal/session.hpp
src/session.cpp
```

Responsibilities:

- logical client session state;
- tunnel/control/video recovery orchestration;
- heartbeat;
- current authenticated session token generation;
- player/video generation lifecycle.

Existing modules retain their focused responsibilities:

- `tunnel.cpp`: zrok process creation/readiness/repair primitives;
- `net.cpp`: TCP/TLS primitives;
- `media.cpp`: capture command/process primitives;
- `input.cpp`: key mapping, held state, mouse normalization;
- `client.cpp`: CLI-facing connection setup and input event capture;
- `host.cpp`: host protocol handlers and host-side process supervision.

The supervisor should depend on these modules rather than duplicate their internals.

## Test-driven acceptance criteria

Production code must be preceded by failing regressions for each behavior.

Required automated coverage:

1. first zrok access attempt fails, subsequent attempt succeeds, and pairing continues without restarting `opal`;
2. first-time pairing password is prompted exactly once;
3. paired reconnect uses `AUTH` without requesting a password;
4. control socket failure triggers AUTH recovery and a new video token;
5. held keys/buttons are released when a control generation dies;
6. video delivery tolerates a network delay greater than one second without truncating the stream;
7. player exit causes an entirely new video generation;
8. capture/video reconnect never appends generation B bytes to generation A player stdin;
9. corrupt/failed video generation is discarded before replacement playback;
10. host zrok child death recreates the same saved token and restarts tunnel sharing;
11. synthetic XI2 resolution around 1000 counts/meter does not amplify pointer movement;
12. plausible physical resolution is normalized correctly;
13. normalization scale is clamped;
14. host cursor capture is explicit;
15. invalid connection-code error includes the expected `opal:CONTROL,VIDEO` form;
16. integration test holds one logical session through player restart, video restart, and control reconnect;
17. full existing clean/install/smoke/integration suites remain green.

## Non-goals

This pass does not replace zrok2, TLS, GPU Screen Recorder, FFplay, or uinput.

It does not implement QUIC/UDP media, native FFmpeg decoding/rendering, clipboard synchronization, file transfer, multi-monitor UI, or Windows/macOS clients.

Those may be future performance/feature work after the current Linux remote-desktop path is stable.

## Success criteria

The stabilization is complete when normal use can survive realistic startup propagation and transient tunnel/video/control interruptions while maintaining a single paired identity and automatically returning to a live remote desktop with working keyboard and appropriately scaled visible mouse input.
