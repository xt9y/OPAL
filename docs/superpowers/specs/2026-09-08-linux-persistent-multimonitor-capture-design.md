# Linux Persistent Multi-Monitor Capture Design

## Goal

Make Linux/Wayland hosting use one persistent portal authorization/session instead of recreating KDE's screen/window chooser during normal capture recovery, and support selecting multiple monitors once and presenting them side-by-side as one remote desktop canvas.

## User-visible behavior

During Linux host setup, OPAL asks whether screen capture should be remembered. If enabled, the host opens the compositor portal once and the user may select one or multiple monitors. OPAL stores the persistent restore token and reuses the authorized monitor set on later host/client sessions.

During an active remote session OPAL must never silently open the KDE/xdg-desktop-portal source chooser. Encoder restarts, bitrate changes, IDR recovery, packet loss, decode recovery, capture-stale recovery, and client reconnects must not trigger interactive screen authorization.

If the stored authorization is no longer restorable, OPAL reports an explicit screen-authorization-lost error and requires deliberate host reauthorization instead of opening the chooser in the background.

## Portal lifecycle

The Linux host owns the XDG ScreenCast portal session rather than VideoSender.

The portal request uses:

- source type: monitor only
- multiple sources: enabled
- cursor mode: embedded
- persistence: persistent
- restore token: previously saved token when present

The initial interactive setup may show the compositor chooser. Normal streaming paths use restoration only.

Every successful portal Start result may return a new restore token. OPAL immediately replaces the previous token with the newest token using an atomic file replacement. The capture session does not recreate the portal merely because the encoder or media sender needs to restart.

## Host capture ownership

Introduce a Linux host capture session abstraction responsible for:

- XdpPortal and XdpSession lifetime
- persistent restore-token rotation
- PipeWire remote lifetime
- discovery of all selected monitor streams
- per-monitor stream state and latest-frame mailboxes
- monitor metadata needed for composition and pointer mapping
- explicit authorization-lost state

VideoSender consumes frames from this already-authorized capture source. Encoder and network lifecycle remain independent from portal authorization lifecycle.

## Multi-monitor capture

Each selected portal stream becomes one MonitorStream. OPAL keeps one latest-frame mailbox per monitor; faster monitors never block slower monitors.

At each compositor output frame, OPAL takes the latest available frame for each authorized monitor. A missing/stale monitor does not force a new portal request.

Selected monitors are ordered deterministically by the portal-reported monitor identity/metadata. New monitors connected later are not automatically added because they were not part of the user's authorization.

## Composition

All selected monitors are normalized into equal-sized tiles.

Tile dimensions are derived from the largest selected source dimensions before final output clamping. Every monitor image is scaled to exactly the same tile width and tile height, matching the requested behavior even when aspect ratios differ.

Tiles are placed horizontally with no gap:

    [ monitor 0 ][ monitor 1 ][ monitor 2 ]

The combined canvas width is tile_width * monitor_count and the height is tile_height.

If the combined canvas exceeds the negotiated encoder/client limit, OPAL uniformly scales the completed canvas down once. Monitor tiles remain equal in final dimensions.

The compositor reuses buffers and scaling contexts to avoid per-frame allocation churn. It must preserve the current latest-frame/low-latency behavior rather than queueing historical monitor frames.

## Capture timestamps

Each monitor frame retains its PipeWire capture timestamp.

For a composed frame, OPAL uses a conservative composite timestamp that does not claim the visible frame is newer than its oldest contributing monitor image. This keeps capture-to-packet latency telemetry meaningful across mixed refresh rates.

## Input mapping

Relative mouse input remains unchanged. The existing relative/raw mouse path and mouse normalization behavior are not modified.

Absolute pointer events are mapped through the inverse compositor transform:

1. client pointer position -> combined remote canvas position
2. combined x coordinate -> selected monitor tile
3. tile-local x/y -> original monitor-local coordinates using inverse tile scaling
4. monitor-local coordinates -> host virtual-desktop coordinates using portal monitor position metadata
5. emit the existing Linux absolute pointer input

This lets OPAL display monitors in one clean horizontal row even if KDE physically arranges them with vertical offsets.

Pointer mapping clamps to the selected monitor's bounds so scaled or mixed-resolution tiles cannot produce out-of-range host coordinates.

## Recovery behavior

### Encoder/network recovery

The following must not recreate portal authorization:

- bitrate reconfiguration
- IDR recovery
- packet/reassembly/decode loss
- send failure
- client reconnect
- encoder restart

These restart only the relevant encoder/media state.

### PipeWire stream recovery

If one monitor stream disconnects, OPAL first attempts to reconnect/recreate that PipeWire stream within the existing authorized portal remote/session.

If a selected monitor physically disappears, OPAL keeps the remaining authorized monitor streams alive. The missing tile may be removed from the composed layout and the encoder configuration is refreshed with a new IDR.

### Portal failure

If the full portal session becomes invalid or restoration is rejected, OPAL enters authorization-lost state. It must not fall back to an interactive portal Start from a live remote media path.

Reauthorization occurs only through an explicit host setup/reconfigure action.

## Existing protocol compatibility

No new multi-monitor video protocol is introduced.

The Linux host composes all authorized monitors into one raw frame and feeds the existing single H.264 encoder. Existing client decode, presentation, encryption, FEC, relay/direct UDP, bitrate feedback, and packetization remain unchanged.

Linux-to-macOS and Linux-to-Linux clients therefore receive the same single H.264 stream contract as before.

## Setup/configuration

Linux first host setup gains a yes/no prompt analogous to Wake-on-LAN:

    Remember selected screens for automatic hosting? [Y/n]

When enabled, host setup performs the one interactive portal authorization required to obtain a persistent restore token.

Configuration records whether automatic screen restoration is enabled. The restore token remains private OPAL state under ~/.opal and is replaced atomically whenever the portal rotates it.

A deliberate reauthorization command/path should remove/replace the old restore token and reopen the portal chooser.

## Error reporting

Distinguish at least:

- portal unavailable
- user denied initial authorization
- stored authorization cannot be restored
- PipeWire remote unavailable
- one monitor stream disconnected
- all monitor streams unavailable
- compositor/encoder reconfiguration failure

When authorization is lost during a remote session, diagnostics should explicitly say to rerun Linux host screen authorization instead of reporting a generic capture failure.

## Testing

Add focused tests/contracts for:

- multiple portal streams are consumed, not only child 0
- restore-token rotation replaces the previous token atomically
- live recovery paths cannot call interactive portal authorization
- encoder restart does not recreate the portal session
- equal tile sizing and horizontal canvas geometry
- mixed-resolution monitor scaling
- combined output clamping preserves equal tile sizes
- conservative composite timestamp selection
- absolute pointer inverse mapping for two and three monitors
- relative mouse path remains unchanged
- monitor removal updates composition without interactive portal fallback
- authorization-lost failure is terminal until explicit reauthorization

Linux integration/runtime verification should include KDE Wayland with one and two selected monitors and repeated bitrate/capture recovery events while asserting that the KDE chooser does not reappear mid-session.

## Out of scope

- separate independent video streams per monitor over the OPAL protocol
- client-side per-monitor windows
- automatically authorizing newly connected monitors
- bypassing the desktop portal's first consent prompt
- changing macOS ScreenCaptureKit behavior
