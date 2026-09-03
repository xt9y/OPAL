# OPAL Low-Latency + Rendezvous Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** First make OPAL's direct-media path measurably low-latency, then replace zrok completely with an OPAL-owned rendezvous/direct-session/relay architecture so ordinary users install OPAL only.

**Architecture:** Preserve direct encrypted UDP as the preferred peer path. Add bounded end-to-end media telemetry, remove cursor feedback from the video stream, correct sender serialization/GOP behavior, isolate UDP receive from decode/presentation, and improve decoder throughput. After the performance gate, add an OPAL rendezvous service, E2E peer handshake, unified direct UDP control/media, relay fallback, and finally delete every zrok runtime/config/documentation dependency.

**Tech Stack:** C++20, OpenSSL EVP/TLS/Ed25519/X25519/HKDF, FFmpeg libavformat/libavcodec/libavutil/libswresample, Linux UDP/recvmmsg, X11/XInput2, OpenGL/GLX, systemd user service.

**Spec:** `docs/superpowers/specs/2026-09-04-opal-low-latency-rendezvous-design.md`

## Global Constraints

- Ordinary users install OPAL and nothing else.
- Final production source contains no zrok runtime dependency.
- Direct encrypted P2P is preferred; relay is fallback only after direct failure.
- Relay never sees OPAL plaintext/session keys.
- No conventional playback/jitter queue.
- Pointer motion is latest-wins and must not block behind reliable control.
- Fresh video wins over stale continuity.
- `OPAL_DEBUG=1` must not print secrets.
- CI remains manual-only.
- Work on `main` as requested by the repository owner.

---

## File map

### Existing files modified during performance work

- `include/opal/video_feedback.hpp` — telemetry structures/parsers.
- `src/video_feedback.cpp` — telemetry serialization/formatting.
- `src/video_sender.cpp` — sender metrics, burst policy, chain/IDR counters.
- `src/video_receiver.cpp` — remote metrics ingestion, RX/media worker split, receive counters.
- `src/media.cpp` — capture command cursor/GOP settings.
- `src/media_profile.cpp` / `include/opal/media_profile.hpp` — pure GOP/burst policy helpers.
- `src/video_decoder.cpp` / `include/opal/video_decoder.hpp` — decoder backend/thread strategy + backend name.
- `src/video_present.cpp` / `include/opal/video_present.hpp` — presented FPS counter.
- `src/udp_transport.cpp` / `include/opal/udp_transport.hpp` — batched receive/kernel overflow metadata.
- `tests/test_video_feedback.cpp`, `tests/test_media.cpp`, `tests/test_media_profile.cpp`, `tests/test_video_decoder.cpp`, `tests/test_direct_video_pipeline.cpp`, `tests/test_udp_transport.cpp` — regression coverage.

### New networking files after performance gate

- `include/opal/rendezvous_protocol.hpp`, `src/rendezvous_protocol.cpp`
- `include/opal/rendezvous_client.hpp`, `src/rendezvous_client.cpp`
- `include/opal/peer_handshake.hpp`, `src/peer_handshake.cpp`
- `include/opal/session_packet.hpp`, `src/session_packet.cpp`
- `include/opal/reliable_control.hpp`, `src/reliable_control.cpp`
- `include/opal/peer_session.hpp`, `src/peer_session.cpp`
- `include/opal/relay_protocol.hpp`, `src/relay_protocol.cpp`
- `server/rendezvous_server.cpp`
- `server/relay_server.cpp`
- matching focused tests under `tests/`.

### Deleted only at final zrok cutover

- `src/tunnel.cpp`
- `src/tunnel_access.cpp`
- `src/tunnel_supervisor.cpp`
- `src/zrok_cleanup.cpp`
- `include/opal/tunnel.hpp`
- `include/opal/tunnel_access.hpp`
- `include/opal/tunnel_supervisor.hpp`
- zrok-specific tests/config/docs.

---

### Task 1: End-to-end media debug telemetry

**Files:**
- Modify: `include/opal/video_feedback.hpp`
- Modify: `src/video_feedback.cpp`
- Modify: `src/video_sender.cpp`
- Modify: `src/video_receiver.cpp`
- Modify: `src/session.cpp`
- Test: `tests/test_video_feedback.cpp`

**Interfaces:**
- Produce `HostMediaDebugSample` with bounded scalar fields only.
- Produce `host_media_debug_line(generation, sample)` and `parse_host_media_debug_line(...)`.
- `VideoSender::handle_control_line` recognizes `DEBUG_MEDIA <generation> <0|1>`.
- `VideoReceiver::handle_control_line` accepts host debug samples and formats them only when local debug mode is enabled.

- [ ] Write failing tests proving a host sample round-trips, rejects wrong generation/extra fields/overflow, and contains no key/token field names.
- [ ] Run `make test-video-feedback` and confirm RED.
- [ ] Implement the bounded telemetry struct + parser/formatter.
- [ ] Have the client send `DEBUG_MEDIA <generation> 1` after media negotiation only when `OPAL_DEBUG=1`.
- [ ] Instrument sender frame bytes, packet/FEC counts, send span, target/active bitrate, stale count, chain state, IDR requests/restarts, and capture-to-packet EWMA.
- [ ] Route one host sample/second over authenticated control; no sample when debug is disabled.
- [ ] Extend client output with host and client lines while preserving current latency line compatibility.
- [ ] Run `make test-video-feedback test-session test-direct-video-pipeline`.
- [ ] Commit `OPAL: add end-to-end media debug telemetry`.

### Task 2: Local cursor feedback and normal GOP policy

**Files:**
- Modify: `src/media.cpp`
- Modify: `include/opal/media_profile.hpp`
- Modify: `src/media_profile.cpp`
- Test: `tests/test_media.cpp`
- Test: `tests/test_media_profile.cpp`

**Interfaces:**
- Produce `normal_gop_frames(int fps)` returning approximately 2 seconds, clamped to valid FPS.
- Host capture commands must use `-cursor no` for GSR and `-draw_mouse 0` for FFmpeg fallback.

- [ ] Add tests asserting no host-captured cursor and a 2-second normal GOP at 60/120 FPS.
- [ ] Run `make test-media test-media-profile` and confirm RED.
- [ ] Implement `normal_gop_frames` and update capture command generation.
- [ ] Keep explicit IDR/restart recovery behavior unchanged.
- [ ] Run tests GREEN.
- [ ] Commit `OPAL: decouple cursor and lengthen normal GOP`.

### Task 3: Frame-aware sender burst budget

**Files:**
- Modify: `include/opal/media_profile.hpp`
- Modify: `src/media_profile.cpp`
- Modify: `src/video_sender.cpp`
- Test: `tests/test_media_profile.cpp`
- Test: `tests/test_direct_video_pipeline.cpp`

**Interfaces:**
- Produce `sender_burst_budget_bytes(int bitrate_kbps, int fps, bool keyframe)`.
- Ordinary burst budget: at least `max(128 KiB, 2 * average frame bytes)` with fixed ceiling.
- Keyframe/recovery burst uses a larger fixed ceiling but never unbounded.

- [ ] Write failing pure-policy tests for 30 Mbps/60 FPS and caps.
- [ ] Run RED.
- [ ] Replace the fixed two-datagram token cap in `VideoSender` with frame-aware allowance.
- [ ] Record `first_send_us`, `last_send_us`, fragment/FEC counts for debug.
- [ ] Add regression that sender remains bounded and does not create a frame queue.
- [ ] Run `make test-media-profile test-direct-video-pipeline test-direct-video-stress`.
- [ ] Commit `OPAL: burst useful frames without queueing`.

### Task 4: Batched UDP receive and kernel-drop counters

**Files:**
- Modify: `include/opal/udp_transport.hpp`
- Modify: `src/udp_transport.cpp`
- Test: `tests/test_udp_transport.cpp`

**Interfaces:**
- Add a bounded `recv_datagrams_batch(...)` API with caller-owned fixed buffers.
- Linux implementation uses `recvmmsg()`; fallback uses existing `recv_datagram()`.
- Expose `SO_RXQ_OVFL` delta when supported.

- [ ] Write loopback burst test that sends many small datagrams and drains more than one in a batch.
- [ ] Run RED.
- [ ] Implement batch receive with no heap allocation in steady state.
- [ ] Add best-effort kernel overflow ancillary-data parsing.
- [ ] Run `make test-udp-transport` GREEN.
- [ ] Commit `OPAL: batch direct UDP receive`.

### Task 5: Split RX from decode/presentation without a playback queue

**Files:**
- Modify: `src/video_receiver.cpp`
- Test: `tests/test_direct_video_pipeline.cpp`
- Test: `tests/test_direct_video_stress.cpp`

**Interfaces:**
- RX worker owns socket, replay window, decrypt, reassembly, sequence/loss accounting.
- Media worker owns decoder and presenter.
- Handoff is bounded to one active access unit plus one key/config recovery slot; no growing deque.
- If the media worker is behind on dependent P-frames, invalidate chain/request IDR rather than accumulating latency.

- [ ] Add a synthetic test hook that deliberately stalls decode/present while packets continue arriving.
- [ ] Assert RX packet count continues increasing during the stall and bounded handoff never exceeds its fixed capacity.
- [ ] Run RED.
- [ ] Implement worker split + condition variable/mailbox.
- [ ] Ensure stop/recovery wakes both workers and cannot deadlock.
- [ ] Run direct pipeline/stress tests GREEN.
- [ ] Commit `OPAL: isolate UDP receive from video decode`.

### Task 6: Decoder backend and throughput policy

**Files:**
- Modify: `include/opal/video_decoder.hpp`
- Modify: `src/video_decoder.cpp`
- Test: `tests/test_video_decoder.cpp`

**Interfaces:**
- Add `std::string backend_name() const`.
- Detect FFmpeg hardware device types at runtime and attempt H.264 hardware configuration only when usable.
- Software fallback chooses a measured low-delay configuration instead of hard-coded one-thread/no-threading.

- [ ] Add tests that backend name is non-empty and software fallback still decodes the existing generated H.264 stream.
- [ ] Add environment override `OPAL_DECODER=software` for deterministic tests/debug only.
- [ ] Run RED.
- [ ] Implement backend selection and safe fallback.
- [ ] Keep corrupt-frame rejection.
- [ ] Run `make test-video-decoder test-direct-video-pipeline` GREEN.
- [ ] Commit `OPAL: select low-latency H264 decoder backend`.

### Task 7: Presentation FPS and live-edge instrumentation

**Files:**
- Modify: `include/opal/video_present.hpp`
- Modify: `src/video_present.cpp`
- Modify: `src/video_receiver.cpp`
- Test: `tests/test_video_present.cpp`

**Interfaces:**
- Add monotonically counted presented frames and rolling FPS snapshot.
- No new frame queue.

- [ ] Write presenter counter test using existing windowed test harness.
- [ ] Implement counters/timestamps without `glFinish()`.
- [ ] Include decoder backend/decoded FPS/presented FPS in debug output.
- [ ] Run relevant tests.
- [ ] Commit `OPAL: expose presentation cadence telemetry`.

### Task 8: Real-machine performance gate

**Files:**
- Update plan checkboxes/results only after evidence.

- [ ] Build/install the same `main` SHA on host and client.
- [ ] Run `OPAL_DEBUG=1 opal` for at least 10 seconds of active pointer/window movement.
- [ ] Capture host frame/send metrics and client RX/reassembly/decode/present metrics.
- [ ] Do not proceed from measurements by assumption: fix the largest measured OPAL-owned stage first if ordinary-frame sender serialization, reassembly, decode, or present still exceeds target.
- [ ] Confirm local cursor feels local even if content return latency remains path-dependent.
- [ ] Mark performance half ready only when telemetry is internally consistent.

### Task 9: Rendezvous protocol types

**Files:**
- Create: `include/opal/rendezvous_protocol.hpp`
- Create: `src/rendezvous_protocol.cpp`
- Create: `tests/test_rendezvous_protocol.cpp`
- Modify: `Makefile`

**Interfaces:**
- Define bounded messages for host lease, introduction request/response, observed endpoint, session nonce, relay allocation request/response.
- Connection code parser/formatter produces short `opal:XXXX-XXXX` style public identifiers.

- [ ] TDD strict parse/serialize/length limits and code checksum/alphabet.
- [ ] No auth/session secrets in connection code.
- [ ] Commit `OPAL: define rendezvous protocol`.

### Task 10: Self-hostable rendezvous server + client

**Files:**
- Create: `include/opal/rendezvous_client.hpp`
- Create: `src/rendezvous_client.cpp`
- Create: `server/rendezvous_server.cpp`
- Create: `tests/test_rendezvous_server.cpp`
- Modify: `Makefile`

**Interfaces:**
- Host registers lease with public rendezvous ID + signed presence proof.
- Client requests introduction to ID.
- Server returns short-lived session nonce + observed endpoints and forwards peer introduction.

- [ ] TDD lease expiry, offline host, unauthorized/malformed introduction, endpoint observation.
- [ ] Server binds only configured address, enforces message/rate bounds.
- [ ] Official endpoint remains configurable at build/runtime for testing/self-hosting; user-facing default requires no configuration.
- [ ] Commit `OPAL: add rendezvous service and client`.

### Task 11: E2E peer handshake independent of zrok/TLS exporter

**Files:**
- Create: `include/opal/peer_handshake.hpp`
- Create: `src/peer_handshake.cpp`
- Modify: `include/opal/video_crypto.hpp`
- Modify: `src/video_crypto.cpp`
- Create: `tests/test_peer_handshake.cpp`

**Interfaces:**
- Ephemeral X25519 keypair per generation.
- Long-term Ed25519 signature over transcript.
- HKDF-SHA256 derives direction-specific session/control/media/probe/relay keys.

- [ ] TDD equal shared keys, wrong peer identity, transcript tamper, replay generation, pairing binding.
- [ ] Erase ephemeral/session secret buffers.
- [ ] Keep old TLS-exporter derivation until final networking cutover tests pass.
- [ ] Commit `OPAL: derive peer session keys end to end`.

### Task 12: Unified direct UDP control/input session

**Files:**
- Create: `include/opal/session_packet.hpp`, `src/session_packet.cpp`
- Create: `include/opal/reliable_control.hpp`, `src/reliable_control.cpp`
- Create: `include/opal/peer_session.hpp`, `src/peer_session.cpp`
- Create: `tests/test_reliable_control.cpp`, `tests/test_peer_session.cpp`

**Interfaces:**
- Packet classes separate latest-wins pointer/media from reliable key/button/config control.
- Reliable messages carry bounded sequence/ACK/retry state.
- Pointer coalescing never waits for reliable retransmission.

- [ ] TDD key/button retransmit and duplicate suppression.
- [ ] TDD pointer latest-wins under simulated control loss.
- [ ] TDD media still flows while a reliable control packet is awaiting ACK.
- [ ] Commit `OPAL: add unified direct peer session`.

### Task 13: NAT traversal through rendezvous

**Files:**
- Modify: `src/direct_video_session.cpp` or replace orchestration with `peer_session.cpp` while reusing `udp_transport.*`.
- Modify: `src/rendezvous_client.cpp`
- Test: `tests/test_peer_session.cpp`

**Interfaces:**
- Candidate priority LAN -> observed/reflexive -> relay.
- Direct deadline remains bounded.

- [ ] TDD LAN selection wins when available.
- [ ] TDD observed public candidate path.
- [ ] TDD direct failure reaches relay request state without user configuration.
- [ ] Commit `OPAL: establish peer sessions through rendezvous`.

### Task 14: Blind E2E relay fallback

**Files:**
- Create: `include/opal/relay_protocol.hpp`, `src/relay_protocol.cpp`
- Create: `server/relay_server.cpp`
- Create: `tests/test_relay.cpp`

**Interfaces:**
- Relay outer envelope contains allocation/session routing ID only.
- Inner peer packet remains OPAL AEAD ciphertext.

- [ ] TDD forced direct failure -> relay connectivity.
- [ ] Assert relay implementation cannot parse/decrypt inner peer payload.
- [ ] Enforce allocation TTL, peer binding, rate and datagram size limits, anti-amplification.
- [ ] Debug reports `path=relay`.
- [ ] Commit `OPAL: add encrypted relay fallback`.

### Task 15: Move SessionSupervisor to OPAL-native networking

**Files:**
- Modify: `src/session.cpp`
- Modify: `src/host.cpp`
- Modify: `src/client.cpp`
- Modify: tests around session/recovery.

**Interfaces:**
- `SessionSupervisor` orchestrates `RendezvousClient` + `PeerSession`; it no longer starts tunnel access.
- Recovery increments generation, performs fresh rendezvous introduction/handshake/path selection, and derives fresh keys.

- [ ] TDD initial pairing, saved identity reconnect, control loss recovery, direct media recovery, stale-generation rejection.
- [ ] Ensure established direct session survives temporary rendezvous outage.
- [ ] Commit `OPAL: migrate session supervisor off tunnels`.

### Task 16: Setup UX and zrok deletion

**Files:**
- Modify: `src/setup.cpp`, `src/system.cpp`, `src/client.cpp`, `src/host.cpp`, `README.md`, `Makefile`, hardening/install tests.
- Delete all zrok/tunnel files listed in the spec.

**Interfaces:**
- Host setup prints a short OPAL connection code.
- Client stores rendezvous ID + pinned identity.
- `doctor` checks only OPAL/system/media requirements, not zrok.
- `clean` removes OPAL state/service registrations only.

- [ ] First add hardening tests that fail if production source/README/Makefile requires `zrok2`, `tunnel_access`, or zrok setup.
- [ ] Run RED.
- [ ] Replace setup/storage paths and remove zrok from build sources.
- [ ] Delete zrok implementation/headers/tests/config migration code.
- [ ] Update README to OPAL-only install/setup.
- [ ] Run `make test-hardening test-setup test-session test-clean test-install` plus full local `make test` when practical.
- [ ] Confirm `.github/workflows/ci.yml` still contains `workflow_dispatch` and no push trigger.
- [ ] Commit `OPAL: remove zrok and complete native networking`.

### Task 17: Final security/stress/real-network validation

- [ ] Run dependency preflight/build on Fedora/Fedora Asahi and at least one second distro family when available.
- [ ] Run direct-media stress and loss/reorder tests.
- [ ] Run peer handshake replay/tamper tests.
- [ ] Run forced-relay integration test.
- [ ] Run real direct connection with `OPAL_DEBUG=1` and confirm path/candidate/latency metrics.
- [ ] Verify no secrets in debug logs.
- [ ] Verify no `zrok2` process/binary/account is required on either machine.
- [ ] Verify `opal setup -> connection code -> opal -> Connected` is the complete normal networking UX.
- [ ] Only then mark this migration complete.