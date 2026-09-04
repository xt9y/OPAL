# OPAL Latency Stabilization Design

## Goal

Eliminate the current IDR/restart feedback loop, restore the real host cursor to captured video, make latency diagnostics identify the responsible stage and recovery cause, and keep OPAL's live-edge/no-playback-buffer behavior.

## Evidence

The failing session is on the direct LAN path. Typical steady samples show capture-to-packet around 0.2-6 ms, network around 2.7-3.1 ms, reassembly around 29-32 ms, software IDR decode around 42-53 ms, and presentation around 5-18 ms. `decode_fps`/`present_fps` repeatedly collapse to 0-1 while `stale`, `idr`, and `restarts` continually increase.

The receiver currently stores only one pending encoded H.264 access unit. While a large IDR is decoding, multiple dependent P-frames can arrive. Replacing/dropping one dependent frame invalidates all later P-frames, so the receiver requests another IDR. The host restarts capture to satisfy that request, making the client repeatedly decode large IDRs instead of reaching cheap steady-state P-frames. The same loop keeps reassembly/decode EWMA values biased toward the slowest frames.

## Architecture

### Dependency-preserving decode backlog

Keep a small fixed-capacity ring of encoded H.264 access units on the receiver. The ring is not a playback queue: frames are decoded immediately in dependency order and intermediate decoded pictures are skipped for presentation whenever newer encoded frames are already waiting. Only presentation is latest-wins; H.264 reference decode remains ordered.

A keyframe clears older pending video because it establishes a new dependency chain. Normal P-frames append. If the fixed backlog actually overflows, OPAL clears it and requests a fresh IDR with an explicit `decode-backlog` cause rather than silently dropping a reference frame.

The backlog must use fixed storage rather than an allocating `std::deque` in the hot path.

### Cursor capture

The real host cursor belongs in the video stream again. gpu-screen-recorder uses `-cursor yes`; the FFmpeg X11 fallback uses `-draw_mouse 1`. OPAL's low-latency input path remains unchanged, but the viewer now sees the real remote cursor position instead of relying on local pointer feedback as a perceptual latency mask.

### Recovery diagnostics

IDR requests carry a compact cause token such as `reassembly-loss`, `decode-failure`, or `decode-backlog`. Host media telemetry reports the most recent capture restart cause. Client latency telemetry reports encoded-video backlog depth and the count of decoded frames deliberately skipped for presentation.

This makes `OPAL_DEBUG=1 opal` sufficient to identify whether a bad session is capture, network, reassembly, decode, presentation, or recovery churn.

### Capture restarts

A requested IDR, material bitrate downshift, capture termination, or genuine capture failure may restart the encoder. Upward bitrate recovery must not restart an otherwise healthy capture just to chase the target bitrate; it can wait until the next necessary restart. This avoids encoder/GOP churn during normal recovery.

### Existing optimizations retained

The stabilization pass must preserve direct encrypted UDP, batched receive, replay protection, reusable crypto buffers, zero-copy encoded access-unit fragmentation, XOR FEC, bounded two-datagram sender burst, accelerated IDR pacing, capture stale-frame rejection, decoder hardware auto-probe with software fallback, stride-aware GL uploads, disabled swap interval, compositor bypass, and bounded audio buffering.

## Success criteria

- Captured video contains the host cursor on both gpu-screen-recorder and FFmpeg fallback paths.
- A 2-4 frame arrival burst during one slow IDR decode does not request a new IDR.
- H.264 access units are decoded in dependency order.
- Intermediate decoded frames may be skipped for presentation to catch up to the live edge.
- Backlog overflow requests one explicit `decode-backlog` IDR instead of silently corrupting dependencies.
- Healthy upward bitrate recovery does not restart capture.
- Debug output exposes backlog depth, skipped presentation frames, and restart/IDR cause.
- Existing sender pacing/FEC/crypto/presenter optimizations remain covered by regression checks.
