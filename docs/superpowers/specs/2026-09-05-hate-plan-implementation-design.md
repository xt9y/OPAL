# OPAL Hate Plan Implementation Design

## Goal

Turn OPAL from a buffered, recovery-heavy remote desktop prototype into a bounded-latency realtime system whose queues, copies, blocking operations, and failure recovery are explicit and measurable.

## Non-negotiable constraints

- Preserve the current OPAL connection model, pairing, rendezvous, relay fallback, Tailscale/LAN discovery, clipboard, audio, keyboard, mouse, and SDL3 client UX.
- Keep C++20, SDL3, FFmpeg libraries, OpenSSL, PulseAudio compatibility, and Linux uinput support.
- Every HPI implementation commit starts with `HPI:`.
- Prefer bounded queues, latest-state semantics, fixed storage, and local recovery over completeness and whole-session restart.
- Never let application callbacks, input injection, decoding, presentation, clipboard, or audio block UDP receive.
- Latency telemetry must describe real acquisition-to-present time and expose queueing separately.

## Architecture

### 1. Realtime media ingress

The current `PeerSession` 512-packet media queue is removed from the media hot path. `recvmmsg()` continues to drain the socket in batches, but media datagrams are handed immediately to a nonblocking media ingress interface. If decoupling is required, the only permitted buffer is a small fixed SPSC ring sized for short scheduler jitter, with drop-oldest semantics and an explicit overflow counter.

Control packets remain authenticated and reliable, but control callbacks are dispatched away from the socket-drain loop. The receive loop only parses/authenticates, records telemetry, and enqueues fixed-size control/input work.

### 2. Latest-frame decode policy

Encoded video backlog is reduced from eight frames to at most two frame slots. Once newer complete video is available, obsolete non-keyframes are discarded before decode. Decoder lag must never accumulate multiple display intervals. Backlog overflow records a drop and requests an IDR only when the dependency chain is actually unusable.

Audio and video have separate scheduling paths so paced video cannot head-of-line block audio or control.

### 3. Capture and encoder lifecycle

The production target is native PipeWire capture with acquisition timestamps taken at frame arrival. DMA-BUF is preferred when available. Hardware encode surfaces are kept on the GPU where practical. `gpu-screen-recorder`/FFmpeg subprocess capture remains a compatibility fallback during migration, not the final primary path.

The encoder remains alive for the session. Bitrate changes and IDR requests must be applied to the live encoder where the selected backend permits it. Capture/encoder restart is reserved for actual backend failure, EOF, or unrecoverable reconfiguration.

During the compatibility phase, subprocess capture timestamps must be treated as estimated and telemetry must mark them as such rather than presenting them as true capture timestamps.

### 4. Packetization and pacing

Keep the existing 1200-byte datagram ceiling and persistent ChaCha20-Poly1305 contexts. Replace sleep-while-holding-`send_mu` pacing with a dedicated pacer that owns packet scheduling. Media producers submit descriptors; the pacer sends without blocking producers.

Use batched send (`sendmmsg` on Linux) where it provides benefit. `EAGAIN`/`EWOULDBLOCK` is congestion/backpressure, not media-chain corruption. Realtime policy may retry briefly or drop obsolete video, but must not automatically invalidate the decoder chain for a transient socket-buffer condition.

FEC becomes adaptive. Clean LAN paths use zero or minimal redundancy; measured loss enables parity. Existing XOR FEC may remain as the first adaptive implementation, but it must not be unconditional.

### 5. Reassembly and allocation policy

Replace `std::map` plus per-frame vectors with a fixed pool of two or three frame slots. Fragment payload storage and metadata are allocated once per receiver generation. Completion must not rebuild the frame through repeated vector insertion.

The resulting encoded access unit is exposed as contiguous FFmpeg-compatible storage with input padding already available, avoiding the extra compressed-frame copy in `VideoDecoder`.

### 6. Decode and presentation surfaces

Software decode remains low-delay and avoids frame threading. Hardware decode must not immediately transfer every frame GPU-to-CPU if a zero-copy presentation path is supported.

Primary Linux target:

`hardware decode surface -> DMA-BUF / DRM PRIME -> EGLImage/OpenGL texture -> SDL3 swap`

Fallback:

`software frame -> asynchronous PBO upload -> OpenGL texture`

`av_frame_clone()` between decode and presentation is removed from the steady-state path. Ownership moves through a latest-frame mailbox/reference-counted surface. Presentation never queues more than one pending display frame.

### 7. Input path isolation

Fix `VideoPresenter::set_relative_mouse_mode()` so enabling relative mode actually enables SDL relative mouse mode.

Input is removed from the media receive thread. Network input packets are converted into compact fixed binary commands and placed into a small nonblocking input mailbox/ring. Pointer motion uses latest-state coalescing: stale intermediate coordinates may be overwritten; button/key transitions remain reliable and ordered.

The uinput broker stays isolated in its helper process initially, but text `getline`/`istringstream` IPC is replaced with fixed binary messages over the pipe/socket. Input helper failure never stalls socket receive. The broker may be restarted independently.

### 8. Client thread separation

The SDL event/input loop must not perform potentially blocking video upload/presentation work. Presentation gets a dedicated renderer thread or a strictly nonblocking latest-frame handoff. SDL event ownership constraints are respected: window/event operations remain on the required thread while expensive upload work is decoupled where SDL/OpenGL permits it.

Clipboard polling is kept off latency-critical paths and never shares media receive locks.

### 9. Audio latency

Reduce the default audio target from 40 ms toward 10-20 ms with adaptive expansion after underruns. Audio decode/playback remains independent from video decode. Video backlog must never delay audio consumption.

### 10. Congestion control

Replace the simple historical-minimum RTT threshold controller with a delivery-rate/queue-delay aware controller. Inputs include:

- acknowledged/received packet rate,
- loss rate,
- RTT and RTT trend,
- socket/kernel drops,
- receiver decode age,
- receiver video backlog,
- sender pacing delay.

The controller differentiates network loss from local queue overflow. Startup begins below the configured ceiling and ramps rapidly based on measured delivery capacity. LAN paths may use a faster startup profile.

### 11. Recovery model

Replace coarse whole-generation recovery with component-local state machines:

- media transport,
- capture backend,
- encoder,
- decoder,
- audio output,
- input broker,
- control session.

A decoder loss event requests IDR/flush. A capture EOF restarts capture/encoder. A transient UDP send failure does not restart capture. A broken input helper restarts only the helper. Whole peer-session reconnection is reserved for authenticated control/path failure.

Media-stall detection becomes multi-stage: detect short stalls quickly, attempt local media recovery, and only escalate to session recovery after local attempts fail.

### 12. Telemetry

Add explicit timestamps/counters for:

- acquisition,
- encode complete,
- first packet queued,
- first/last packet sent,
- first/last packet received,
- reassembly complete,
- decode start/end,
- renderer handoff,
- upload complete,
- swap complete.

Expose p50/p95/p99 for stage latencies plus:

- media ingress drops,
- kernel socket drops,
- encoded-frame drops,
- decoder superseded frames,
- presentation superseded frames,
- input queue overwrites,
- audio queued milliseconds,
- pacing delay.

Telemetry must distinguish measured acquisition timestamps from fallback-estimated timestamps.

## Testing

### Correctness

- Replace source-string architecture assertions with behavioral tests.
- Keep crypto/replay/reassembly unit coverage.
- Add bounded-queue tests proving drop-oldest/latest-frame behavior.
- Add tests proving input callbacks cannot block media receive.
- Add `EAGAIN` send-path tests proving transient backpressure does not invalidate the video chain.
- Add live bitrate/IDR tests that do not restart capture when the backend supports reconfiguration.

### Performance and fault injection

Add realistic 1080p60 and 1440p120 test profiles with frame sizes matching configured bitrate rather than 3-6 KB synthetic frames.

Linux integration testing uses `tc netem` where available for:

- 0-10% random loss,
- burst loss,
- 5-100 ms jitter,
- bandwidth collapse and recovery,
- reordering,
- temporary socket backpressure.

Add simultaneous high-rate pointer input, clipboard activity, audio, CPU pressure, decoder stalls, and capture restarts.

### Sanitizers

Create actual builds for:

- AddressSanitizer + UndefinedBehaviorSanitizer,
- ThreadSanitizer where supported.

`test-direct-media-sanitize` must compile and run sanitizer binaries rather than aliasing normal tests.

### Soak

Add an opt-in long soak that runs for at least one hour with periodic network faults and validates bounded memory, bounded queue depth, continued input responsiveness, and recovery without process restart.

## Implementation order

1. Fix relative mouse mode and decouple input callbacks from UDP receive.
2. Remove the 512-packet media queue / replace with bounded immediate ingress.
3. Reduce encoded-video backlog to latest-frame semantics.
4. Correct `EAGAIN` handling and move pacing out of the sender lock; add batched send.
5. Replace reassembly allocations with fixed slots and eliminate compressed-frame copy.
6. Remove steady-state decoded-frame cloning and add a latest-frame ownership mailbox.
7. Lower/adapt audio queueing.
8. Make FEC adaptive and replace congestion heuristics.
9. Split recovery into component-local recovery paths.
10. Expand telemetry and make capture-time provenance explicit.
11. Add real sanitizer builds and hostile network/performance tests.
12. Introduce native PipeWire/DMA-BUF capture and zero-copy decode/presentation, retaining subprocess/CPU fallbacks.

## Success criteria

- UDP receive is never blocked by input, decode, presentation, audio, clipboard, or capture callbacks.
- No queue in the video path can hide more than roughly two frame intervals of work.
- Transient socket backpressure does not cause capture restart or decoder-chain invalidation.
- Pointer motion remains responsive while video decode/render is intentionally stalled.
- 1080p60 remains bounded under realistic jitter/loss rather than accumulating latency.
- Hardware decode no longer performs the unconditional GPU-to-CPU-to-GPU roundtrip on supported Linux stacks.
- Sanitizer targets are genuine sanitizer builds.
- Debug output can attribute latency to a specific stage and report p50/p95/p99 rather than a single misleading EWMA.
