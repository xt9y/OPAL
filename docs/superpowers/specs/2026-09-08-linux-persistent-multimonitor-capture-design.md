# Linux Persistent Multi-Monitor Capture Design

## Goal

Make Linux/Wayland hosting use one persistent portal authorization/session instead of recreating KDE's screen/window chooser during normal capture recovery, and support selecting multiple monitors once and presenting them side-by-side as one remote desktop canvas.

## User-visible behavior

During Linux host setup, OPAL asks whether screen capture should be remembered. If enabled, the host opens the compositor portal once and the user may select one or multiple monitors. OPAL stores the persistent restore token and reuses the authorized monitor set on later host sessions.

During an active remote session OPAL must never silently open the KDE/xdg-desktop-portal source chooser. Encoder restarts, bitrate changes, IDR recovery, packet loss, decode recovery, capture-stale recovery, and client reconnects must not trigger interactive screen authorization.

If stored authorization cannot be restored, OPAL reports an explicit screen-authorization-lost/not-ready state to remote clients. Reauthorization is deliberate host-side setup work; media recovery never invokes it.

## Portal lifecycle

The Linux host service owns the XDG ScreenCast portal session rather than VideoSender.

The portal request uses:

- output type: `XDP_OUTPUT_MONITOR`
- screencast flags: `XDP_SCREENCAST_FLAG_MULTIPLE`
- cursor mode: embedded
- persistence: `XDP_PERSIST_MODE_PERSISTENT`
- restore token: previously saved token when present

The initial interactive setup may show the compositor chooser. Normal streaming paths do not create portal sessions.

At host service startup/login, before the host advertises capture readiness to remote clients, OPAL establishes or restores the persistent portal session. This is the only automatic lifecycle point allowed to call portal session creation. A valid restore token should restore the selected monitors without asking again. If the desktop portal invalidated/revoked the token or a previously selected source no longer exists, the portal may require host interaction; this happens outside an active remote media session and OPAL remains not-ready until authorization is restored.

Every successful portal Start result may return a new restore token. OPAL immediately replaces the previous token with the newest token using an atomic file replacement because restore tokens are single-use.

The capture session does not recreate the portal because an encoder, VideoSender, PipeWire stream, or client session restarts.

## Host capture ownership

Introduce a Linux host capture session abstraction responsible for:

- XdpPortal and XdpSession lifetime
- persistent restore-token rotation
- PipeWire remote lifetime
- discovery of all selected monitor streams
- per-monitor stream state and latest-frame mailboxes
- monitor metadata needed for composition and pointer mapping
- capture readiness state
- explicit authorization-lost state

VideoSender consumes frames from this already-authorized capture source. Encoder and network lifecycle remain independent from portal authorization lifecycle.

## Multi-monitor capture

Each selected portal stream becomes one MonitorStream. OPAL consumes every returned stream instead of only stream child 0.

OPAL keeps one latest-frame mailbox per monitor; faster monitors never block slower monitors. At each compositor output frame, OPAL takes the latest available frame for each authorized monitor. A missing/stale monitor does not force a new portal request.

For stream identity/ordering, OPAL prefers portal metadata in this order:

1. persistent stream `id` when available
2. monitor `position`/`size` logical metadata
3. `pipewire-serial` for PipeWire targeting when available
4. stable fallback order from the returned stream list

`position`, `size`, `id`, and `pipewire-serial` are optional depending on portal version/backend. OPAL must not assume they always exist.

When position metadata exists, visible tile ordering is left-to-right by compositor logical x position, then y position. Otherwise the stable stream identity/list order is used.

New monitors connected later are not automatically added because they were not part of the user's authorization.

## Composition

All selected monitors are normalized into equal-sized tiles.

Tile dimensions are derived from the largest selected source dimensions before final output clamping. Every monitor image is scaled to exactly the same tile width and tile height, matching the requested behavior even when aspect ratios differ.

Tiles are placed horizontally with no gap:

    [ monitor 0 ][ monitor 1 ][ monitor 2 ]

The combined canvas width is tile_width * monitor_count and the height is tile_height.

If the combined canvas exceeds the negotiated encoder/client limit, OPAL uniformly scales the completed canvas down once. Monitor tiles remain equal in final dimensions.

The compositor reuses frame buffers and scaling contexts. It must not allocate large per-frame composition buffers or queue historical monitor frames.

## Capture timestamps

Each monitor frame retains its PipeWire capture timestamp.

For a composed frame, OPAL uses the oldest timestamp among the monitor frames actually visible in that composite. This prevents latency telemetry from claiming the complete displayed canvas is newer than one of its component images.

## Input mapping

Relative mouse input remains unchanged. The existing relative/raw mouse path and mouse normalization behavior are not modified.

Absolute pointer events are mapped through the inverse compositor transform.

When portal monitor `position` and `size` metadata are available, they are treated as compositor logical coordinates, not pixel coordinates:

1. client pointer -> combined remote canvas
2. combined x -> selected equal-sized tile
3. tile-local x/y -> that monitor's logical `size`
4. add the monitor logical `position`
5. normalize the resulting point across the bounding rectangle of all selected logical monitor regions
6. emit the existing Linux absolute uinput pointer coordinates

This lets OPAL display monitors in one clean horizontal row while still targeting a KDE layout that may contain vertical offsets or different logical scaling.

When portal position/size metadata are absent, OPAL first attempts to resolve equivalent logical display bounds through the local display backend. If reliable monitor geometry still cannot be established, relative mouse input continues to work, but OPAL must not pretend absolute cross-monitor mapping is exact; diagnostics report degraded absolute mapping instead of using guessed pixel geometry.

Pointer mapping clamps to the selected monitor's bounds so stretched/mixed-resolution tiles cannot produce out-of-range host coordinates.

## Recovery behavior

### Encoder/network recovery

The following must not recreate portal authorization or the host capture session:

- bitrate reconfiguration
- IDR recovery
- packet/reassembly/decode loss
- send failure
- client reconnect
- encoder restart

These restart only the relevant encoder/media state.

### PipeWire stream recovery

If one monitor stream disconnects, OPAL attempts to reconnect/recreate that PipeWire stream against the already-open authorized PipeWire remote/session.

If a selected monitor physically disappears, OPAL keeps the remaining authorized monitor streams alive. The missing tile is removed from the composed layout, the encoder configuration is refreshed, and a new IDR is generated. No portal chooser is opened.

If all monitor PipeWire streams become unavailable while the portal session is still valid, OPAL reports capture-unavailable and attempts noninteractive PipeWire recovery inside the existing portal session.

### Portal failure

If the full portal session becomes invalid during a remote session, OPAL enters authorization-lost/not-ready state. It must not call portal session creation from the remote media path.

The active remote media session ends cleanly with an explicit authorization error. Reauthorization occurs through host setup/service readiness handling, not silently from VideoSender.

## Existing protocol compatibility

No new multi-monitor video protocol is introduced.

The Linux host composes all authorized monitors into one raw frame and feeds the existing single H.264 encoder. Existing client decode, presentation, encryption, FEC, relay/direct UDP, bitrate feedback, and packetization remain unchanged.

Linux-to-macOS and Linux-to-Linux clients therefore receive the same single H.264 stream contract as before.

## Setup/configuration

Linux first host setup gains a yes/no prompt analogous to Wake-on-LAN:

    Remember selected screens for automatic hosting? [Y/n]

When enabled, host setup performs the one interactive portal authorization required to obtain persistent screen authorization.

Configuration records whether automatic screen restoration is enabled. The restore token remains private OPAL state under `~/.opal` and is replaced atomically whenever the portal rotates it.

The Linux host service restores screen capture readiness during startup before accepting remote capture clients.

A deliberate reauthorization path removes/replaces the old restore token and reopens the portal chooser.

## Error reporting

Distinguish at least:

- portal unavailable
- user denied initial authorization
- stored authorization cannot be restored / host interaction required
- PipeWire remote unavailable
- one monitor stream disconnected
- all monitor streams unavailable
- absolute pointer geometry degraded
- compositor/encoder reconfiguration failure

When authorization is lost during a remote session, diagnostics explicitly say to redo Linux host screen authorization instead of reporting a generic capture failure.

## Testing

Add focused tests/contracts for:

- `XDP_SCREENCAST_FLAG_MULTIPLE` is requested with monitor-only sources
- multiple portal streams are consumed, not only child 0
- optional stream metadata parsing (`id`, `position`, `size`, `pipewire-serial`)
- restore-token rotation replaces the previous token atomically
- live recovery paths cannot call portal session creation
- encoder restart does not recreate the host portal/capture session
- client reconnect reuses the existing host capture session
- equal tile sizing and horizontal canvas geometry
- mixed-resolution monitor scaling
- combined output clamping preserves equal tile sizes
- oldest-visible-frame composite timestamp selection
- absolute pointer inverse mapping for two and three monitors with logical offsets/scaling
- missing geometry produces explicit degraded absolute mapping rather than guessed geometry
- relative mouse path remains unchanged
- monitor removal updates composition without interactive portal fallback
- authorization-lost failure ends media without opening a chooser

Linux integration/runtime verification includes KDE Wayland with one and two selected monitors, host-service restart, client reconnects, repeated bitrate/IDR/capture recovery events, mixed monitor resolutions/refresh rates, and monitor removal while asserting that the KDE chooser does not reappear during an active remote session.

## Out of scope

- separate independent video streams per monitor over the OPAL protocol
- client-side per-monitor windows
- automatically authorizing newly connected monitors
- bypassing the desktop portal's initial/revoked-permission consent behavior
- changing macOS ScreenCaptureKit behavior
