# OPAL End-to-End Low-Latency Pipeline Design

## Goal

Reduce real mouse-to-photon and screen-to-photon latency, not merely perceived cursor latency. Preserve OPAL's current direct encrypted UDP product behavior while removing whole-frame pipeline delays that remain after the HPI stabilization work.

## Hard constraints

- Keep current pairing, rendezvous/direct/relay behavior, clipboard, audio, keyboard, mouse, and SDL3 client behavior.
- Work on `main`; every implementation commit starts with `HPI:`.
- Keep a usable compatibility path throughout migration.
- Do not reintroduce a fake local cursor. The captured host cursor remains authoritative.
- Keep latest-first bounded queues; never trade lower nominal latency for unbounded buffering.
- Do not require GitHub Actions for validation.
- Native Linux fast paths are preferred; subprocess/CPU-copy paths remain fallback when capabilities are absent.

## 1. Latency attribution

The current sender timestamp is reconstructed after encoded FLV reaches OPAL, so it cannot measure capture + encoder + mux delay. Add explicit stage telemetry that distinguishes:

- acquisition timestamp quality: exact vs estimated,
- capture-to-encoded availability,
- encoded-to-first-packet,
- first-packet-to-complete-frame,
- decode,
- decoded-to-submit,
- upload/import,
- swap return,
- total estimated media age.

Telemetry must expose p50/p95/p99 and state whether acquisition/presentation timestamps are estimates. Debug output must make an invisible fixed capture delay obvious instead of reporting a falsely small total.

## 2. LAN transmission

Keep congestion-aware pacing for WAN/relay paths, but do not deliberately spread a complete encoded frame over many milliseconds on a clean direct/LAN path.

Production direct media sending should support frame-local batching using Linux `sendmmsg()` and the existing 1200-byte datagram ceiling. For low-latency direct paths:

- encrypt/packetize a frame into bounded reusable datagram storage,
- submit ready datagrams in batches,
- treat `EAGAIN`, `EWOULDBLOCK`, and `ENOBUFS` as backpressure,
- drop obsolete ordinary video rather than blocking a newer frame,
- retain controlled pacing for relay/WAN or measured congestion.

Keyframes may use a larger bounded burst but must not create unbounded socket queues.

## 3. Native capture

Final preferred Linux capture path:

`PipeWire -> DMA-BUF when negotiated -> in-process FFmpeg hardware encoder -> H.264 access units -> OPAL packetizer`

Compatibility fallback:

`gpu-screen-recorder/FFmpeg subprocess -> FLV -> native FLV parser`

The native backend must:

- use PipeWire frame callbacks and acquisition timestamps taken when the frame buffer is delivered,
- negotiate common raw formats and DMA-BUF where available,
- keep the encoder alive for the session,
- request no B-frames and low-delay encoder settings,
- expose exact acquisition timestamps,
- fall back cleanly if PipeWire, the portal/session, DMA-BUF, or a hardware encoder cannot be used.

PipeWire support is optional at build time so OPAL remains buildable on systems without PipeWire development files.

## 4. Decode and presentation

Current hardware decode copies hardware frames to CPU via `av_hwframe_transfer_data`, followed by CPU/PBO upload. Preferred Linux fast path:

`H.264 -> FFmpeg DRM PRIME/VAAPI hardware frame -> DMA-BUF -> EGLImage -> GL texture -> swap`

The decoder must preserve an `AV_PIX_FMT_DRM_PRIME` frame when available instead of forcing CPU transfer. The presenter imports compatible `AVDRMFrameDescriptor` planes with `EGL_EXT_image_dma_buf_import`/modifier support when available.

Fallback remains:

`hardware decode -> CPU transfer` or `software decode -> PBO/direct GL upload`.

Zero-copy import failure for a frame must fall back safely rather than killing the session.

## 5. Presentation semantics

Keep swap interval 0. Verify and report whether immediate swap was accepted. Debug output must report the presentation mode as `immediate-requested`, `immediate-active`, or `fallback` instead of silently ignoring failure.

Actual scanout time is platform/compositor dependent; OPAL must label swap-return timing as a submit measurement, not exact photon time.

## 6. High-FPS behavior

Do not hard-switch every machine to 120 Hz. Keep explicit `--fps` overrides. Add a latency-oriented automatic FPS selector that chooses the highest sensible rate from 60/90/120/144/165/240 bounded by the detected local display refresh and host/capture capability, with 60 as compatibility fallback.

Capture staleness, bitrate, and pacing budgets continue to derive from the negotiated FPS.

## 7. Input path

Pointer packets already bypass reliable retransmission and host pointer work is coalesced. Preserve that. Do not spend the migration on sub-millisecond input micro-optimizations while the visible host cursor still pays the video pipeline latency.

The existing binary `opal-input` helper remains acceptable initially. A later in-process uinput broker is permitted only if measurements show the helper contributes material latency.

## 8. Validation

Each migration stage gets focused tests before production changes.

Required coverage:

- stage timestamp math and exact/estimated labels,
- direct batch UDP send including partial `sendmmsg`/backpressure behavior,
- capture backend selection and native-fallback behavior,
- DRM PRIME frame preservation and CPU fallback,
- presenter capability selection without requiring EGL support in headless tests,
- FPS selector boundaries,
- existing direct pipeline, sanitizer, netem resilience, and soak gates.

Success means lower measured sender capture-to-packet latency and no added queue depth, while preserving session recovery behavior.