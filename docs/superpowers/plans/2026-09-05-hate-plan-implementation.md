# OPAL Hate Plan Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove OPAL's hidden latency reservoirs and failure cascades, then replace the remaining copy-heavy capture/decode path with bounded, observable realtime components.

**Architecture:** Keep the authenticated peer protocol and existing product behavior, but make the data path latest-first and component-local. UDP receive only drains/authenticates and dispatches bounded work; input, decode, presentation, audio, capture recovery, and sender pacing are isolated so none can stall socket receive.

**Tech Stack:** C++20, Linux UDP/recvmmsg/sendmmsg, SDL3/OpenGL, FFmpeg/libavcodec/libavformat/libavutil, OpenSSL ChaCha20-Poly1305, PulseAudio, Linux uinput, PipeWire/DMABUF where available.

**Spec:** `docs/superpowers/specs/2026-09-05-hate-plan-implementation-design.md`

## Global Constraints

- Preserve pairing, rendezvous/relay fallback, LAN/Tailscale discovery, clipboard, audio, keyboard, mouse, and SDL3 client behavior.
- Every implementation commit starts with `HPI:`.
- Work directly on `main` as explicitly requested by the user.
- No hot-path queue may accumulate more than roughly two video frame intervals.
- UDP receive must never call blocking application/input/media work.
- Transient `EAGAIN`/`EWOULDBLOCK` must not invalidate the H.264 chain.
- Prefer fixed-capacity storage and latest-state/drop-obsolete semantics.
- Keep subprocess capture and CPU upload as fallbacks until native PipeWire/DMABUF paths are usable.

---

### Task 1: Input isolation and relative mouse correctness

**Files:**
- Modify: `src/video_present.cpp`
- Modify: `src/peer_session.cpp`
- Modify: `src/host.cpp`
- Modify: `src/input_helper.cpp`
- Modify: corresponding headers under `include/opal/`
- Test: `tests/test_peer_session.cpp` and/or new focused input-path test

**Produces:** a receive loop that never performs uinput pipe writes, correct SDL relative mode, binary helper IPC, and coalesced pointer motion.

- [ ] Add failing behavior tests for relative-mode state and nonblocking input dispatch.
- [ ] Fix `SDL_SetWindowRelativeMouseMode(window, enabled)` and preserve grab/cursor semantics.
- [ ] Add bounded control/input dispatcher state owned by `PeerSession` and move callbacks off the UDP receive thread.
- [ ] Replace host-to-helper textual commands with fixed binary input records; keep compatibility only where required for tests/tools.
- [ ] Coalesce pointer motion while preserving ordered key/button transitions.
- [ ] Verify targeted tests/build where an execution environment is available.
- [ ] Commit with `HPI:` prefix.

### Task 2: Remove hidden media packet buffering

**Files:**
- Modify: `src/peer_session.cpp`
- Modify: `include/opal/peer_session.hpp`
- Modify: peer/session tests

**Produces:** immediate or tiny bounded media ingress with explicit overflow telemetry instead of the 512-packet queue.

- [ ] Add a test that floods media ingress while downstream work is stalled and asserts bounded depth/drop accounting.
- [ ] Remove `kPeerMediaQueueCapacity=512` and the packet-copying media worker queue.
- [ ] Dispatch media via an immediate nonblocking ingress contract or a <=32-slot fixed SPSC ring using drop-oldest semantics.
- [ ] Expose media-ingress drop counter in debug telemetry.
- [ ] Verify and commit `HPI:`.

### Task 3: Latest-frame receiver semantics

**Files:**
- Modify: `src/video_receiver.cpp`
- Modify: `include/opal/video_receiver.hpp`
- Test: receiver backlog/architecture tests

**Produces:** at most two encoded frame slots, pre-decode supersession, and IDR only when dependency recovery is required.

- [ ] Replace source-string architecture assertions with behavior tests.
- [ ] Reduce encoded decode backlog from 8 to 2.
- [ ] Drop obsolete non-keyframes before decode when a newer complete frame exists.
- [ ] Track superseded-before-decode separately from corrupt/lost dependency-chain drops.
- [ ] Verify and commit `HPI:`.

### Task 4: UDP backpressure and sender pacing

**Files:**
- Modify: `src/udp_transport.cpp`
- Modify: `include/opal/udp_transport.hpp`
- Modify: `src/video_sender.cpp`
- Modify: sender/transport tests

**Produces:** transient-backpressure-aware UDP sends and sender pacing that does not sleep while holding the shared sender lock.

- [ ] Add tests for `EAGAIN` classification and chain preservation.
- [ ] Introduce a send result enum distinguishing sent, would-block, and fatal.
- [ ] Preserve existing bool wrapper only for non-realtime callers if needed.
- [ ] Move pacing waits outside frame-wide locking; keep packet sequence/encryption ownership safe.
- [ ] Add Linux batched send helper where practical.
- [ ] Verify and commit `HPI:`.

### Task 5: Fixed reassembly and compressed-copy removal

**Files:**
- Modify: `src/video_reassembly.cpp`
- Modify: `include/opal/video_reassembly.hpp`
- Modify: `src/video_decoder.cpp`
- Test: reassembly/FEC/decoder input tests

**Produces:** fixed inflight slots and contiguous padded encoded units that can be handed to FFmpeg without another full-frame memcpy.

- [ ] Add tests for slot reuse, out-of-order fragments, FEC recovery, and bounded memory.
- [ ] Replace `std::map`/per-frame vectors with 2-3 preallocated frame slots and flat fragment metadata.
- [ ] Store fragment payload directly in final contiguous access-unit storage.
- [ ] Add FFmpeg input padding to completed units.
- [ ] Remove decoder's redundant packet staging copy when ownership/lifetime permits.
- [ ] Verify and commit `HPI:`.

### Task 6: Decoded-frame ownership and presentation latency

**Files:**
- Modify: `src/video_decoder.cpp`
- Modify: `src/video_receiver.cpp`
- Modify: `src/video_present.cpp`
- Modify: relevant headers
- Test: latest-frame ownership tests

**Produces:** one pending decoded frame/surface, no steady-state `av_frame_clone`, and cached drawable sizing.

- [ ] Add ownership/supersession tests.
- [ ] Replace clone-based publish with move/reference ownership through a single latest-frame mailbox.
- [ ] Cache drawable dimensions from SDL window events or explicit resize updates rather than querying every frame.
- [ ] Add asynchronous PBO upload fallback for software-decoded frames where supported.
- [ ] Verify and commit `HPI:`.

### Task 7: Audio, adaptive FEC, and congestion control

**Files:**
- Modify: `src/audio_output.cpp`
- Modify: `src/video_packet.cpp`
- Modify: `src/video_sender.cpp`
- Modify: `src/video_feedback.cpp`
- Modify: `src/media_profile.cpp`
- Test: audio/FEC/feedback tests

**Produces:** 10-20 ms adaptive audio buffering, loss-driven FEC, and delivery/queue-aware bitrate behavior without capture restarts.

- [ ] Add behavior tests for audio target growth/shrink and FEC enable thresholds.
- [ ] Lower default audio target and grow only after underrun evidence.
- [ ] Disable/minimize FEC on clean paths; enable parity from measured loss/burst evidence.
- [ ] Separate local queue/kernel drops from reported network loss.
- [ ] Start below bitrate ceiling and ramp using delivery rate + RTT trend + receiver age/backlog.
- [ ] Ensure bitrate reductions and IDR requests do not restart capture merely to reconfigure.
- [ ] Verify and commit `HPI:`.

### Task 8: Component-local recovery and honest telemetry

**Files:**
- Modify: `src/session.cpp`
- Modify: `src/video_sender.cpp`
- Modify: `src/video_receiver.cpp`
- Modify: `src/video_capture.cpp`
- Modify: debug/stat headers and call sites
- Test: recovery/fault tests

**Produces:** media/input/capture/decoder recovery that escalates independently and stage telemetry that labels estimated capture timestamps.

- [ ] Add fault tests proving input-helper failure, decode loss, capture EOF, and transient send backpressure do not all cause full session reconnect.
- [ ] Make short media stalls request local IDR/flush first; escalate only after local recovery fails.
- [ ] Restart capture only on backend failure/EOF or unsupported live reconfiguration.
- [ ] Add stage timestamps/counters and p50/p95/p99 aggregation for acquisition/encode/send/receive/reassembly/decode/handoff/upload/swap.
- [ ] Mark subprocess capture acquisition time as estimated.
- [ ] Verify and commit `HPI:`.

### Task 9: Real sanitizers and hostile performance tests

**Files:**
- Modify: `Makefile.core`
- Modify/create: `tests/test_direct_video_stress.cpp`
- Create: focused queue/backpressure/recovery tests as needed

**Produces:** actual ASan+UBSan/TSan binaries and realistic 1080p60/1440p120 stress/fault coverage.

- [ ] Replace sanitizer alias target with real sanitizer compile/link flags.
- [ ] Scale synthetic payloads from configured bitrate/fps instead of 3-6 KiB frames.
- [ ] Add high-rate input concurrent with media stalls.
- [ ] Add optional `tc netem` scenarios for loss, burst loss, jitter, reordering, bandwidth collapse, and temporary backpressure.
- [ ] Add opt-in one-hour soak target with bounded-memory/queue assertions.
- [ ] Verify and commit `HPI:`.

### Task 10: Native PipeWire/DMABUF and zero-copy Linux path

**Files:**
- Create/modify: capture backend files under `src/` and `include/opal/`
- Modify: `src/media.cpp`, `src/video_capture.cpp`, `src/video_decoder.cpp`, `src/video_present.cpp`
- Modify: build configuration
- Test: backend selection/fallback tests

**Produces:** native acquisition timestamps and GPU-native capture/decode presentation on supported Linux systems while retaining compatibility fallbacks.

- [ ] Add backend-selection tests proving graceful fallback when PipeWire/DMABUF capabilities are absent.
- [ ] Add native PipeWire capture backend with frame-acquisition timestamps.
- [ ] Import DMABUF-capable capture surfaces into the selected hardware encoder when supported.
- [ ] Export/import hardware decode surfaces through DRM PRIME/DMABUF/EGLImage for OpenGL presentation when supported.
- [ ] Retain software decode/PBO and subprocess capture fallbacks.
- [ ] Report active capture/decoder/presentation backend and whether the path is zero-copy.
- [ ] Verify and commit `HPI:`.

## Final verification

- [ ] Inspect every HPI commit and compare `main` against the design-spec base commit.
- [ ] Run the full local test suite, sanitizer suite, and available integration tests in an execution environment.
- [ ] Confirm debug telemetry exposes bounded queues/drops and latency-stage percentiles.
- [ ] Confirm no implementation commit lacks the `HPI:` prefix.
