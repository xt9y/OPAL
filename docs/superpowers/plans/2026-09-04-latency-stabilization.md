# OPAL Latency Stabilization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore visible remote cursor capture and remove the H.264 IDR/restart loop while making OPAL's latency telemetry identify the exact recovery cause.

**Architecture:** Preserve ordered H.264 dependency decode in a small fixed receiver ring while keeping presentation latest-wins. Add recovery-cause telemetry, avoid capture restarts for healthy bitrate increases, and retain the existing direct-UDP/pacing/FEC/crypto/GL low-latency optimizations.

**Tech Stack:** C++20, FFmpeg libavcodec/libavutil, gpu-screen-recorder, X11/GLX, UDP, OpenSSL EVP, existing OPAL test binaries.

**Spec:** `docs/superpowers/specs/2026-09-04-latency-stabilization-design.md`

## Global Constraints

- Work directly on `main` as explicitly approved by the user.
- Do not reintroduce zrok or any legacy tunnel path.
- Do not add a playback buffer.
- H.264 dependent access units must never be silently dropped while later dependent frames are kept.
- Keep sender burst capacity bounded to two datagrams.
- Keep CI manual-only; verify targeted tests locally where dependencies permit.

---

### Task 1: Update the OPAL website to current architecture

**Files:**
- Modify: `xt9y/xt9y-portfolio:opal.html`

**Interfaces:**
- Consumes: current OPAL `main` behavior and the `NEW!` badge styling from `rendercheck.html`/`c.html`.
- Produces: current public documentation with no legacy zrok terminology.

- [ ] **Step 1:** Read the current OPAL architecture, commands, media, recovery, and server behavior from `main`.
- [ ] **Step 2:** Add the shared green `NEW!` badge CSS and mark newly changed networking/media/latency sections.
- [ ] **Step 3:** Replace stale documentation with signed LAN discovery, rendezvous/direct traversal, blind relay fallback, encrypted UDP media/control, H.264/AAC, FEC, pacing, decoder fallback, GLX presentation, and telemetry.
- [ ] **Step 4:** Verify the final HTML contains no `zrok` token and all navigation anchors resolve.
- [ ] **Step 5:** Commit the website update on `xt9y/xt9y-portfolio/main`.

### Task 2: Restore the real cursor to video capture

**Files:**
- Modify: `tests/test_media.cpp`
- Modify: `src/media.cpp`

**Interfaces:**
- Consumes: `capture_command(bool,int,int,bool,const std::string&,int,int)`.
- Produces: GSR command containing `-cursor yes`; FFmpeg fallback containing `-draw_mouse 1`.

- [ ] **Step 1: Write the failing test**

Change the capture assertions so GSR requires `-cursor yes` and rejects `-cursor no`, and FFmpeg requires `-draw_mouse 1` and rejects `-draw_mouse 0`.

- [ ] **Step 2: Run test to verify it fails**

Run the media test against the old `src/media.cpp`; expected failure is the cursor assertion.

- [ ] **Step 3: Write minimal implementation**

Change only the two capture flags in `src/media.cpp`.

- [ ] **Step 4: Run test to verify it passes**

Run the same media test; expected PASS.

- [ ] **Step 5: Commit**

Commit test then implementation using focused messages.

### Task 3: Preserve H.264 dependencies across a slow IDR decode

**Files:**
- Modify: `tests/test_video_receiver_architecture.cpp`
- Modify: `src/video_receiver.cpp`

**Interfaces:**
- Consumes: `MediaItem`, `decoder.decode_latest(...)`, receiver media mutex/CV.
- Produces: fixed-capacity pending-video ring, ordered decode, latest-wins presentation.

- [ ] **Step 1: Write the failing test**

Require fixed pending-video storage (`std::array<std::optional<MediaItem>,...>`), explicit backlog capacity, no single `std::optional<MediaItem> video_frame`, and a decode path that can skip presentation without requesting IDR merely because another P-frame arrived.

- [ ] **Step 2: Run test to verify it fails**

Compile/run the source-architecture regression against the old receiver; expected failure is the missing fixed ring.

- [ ] **Step 3: Write minimal implementation**

Use a fixed ring. Keyframe clears the ring and becomes the new head. P-frames append while capacity remains. Full-ring overflow clears pending video and requests `decode-backlog` IDR. The media thread pops frames in order; after each successful decode it checks whether newer encoded video is waiting and skips GL presentation when catch-up is required.

- [ ] **Step 4: Run test to verify it passes**

Run the architecture test and any receiver/unit tests available.

- [ ] **Step 5: Commit**

Commit test then implementation.

### Task 4: Add actionable recovery telemetry

**Files:**
- Modify: `include/opal/video_feedback.hpp`
- Modify: `src/video_feedback.cpp`
- Modify: `src/video_receiver.cpp`
- Modify: `src/video_sender.cpp`
- Modify: `tests/test_video_feedback.cpp`

**Interfaces:**
- Consumes: `REQUEST_IDR`, `HOST_MEDIA`, `LatencyTelemetry`.
- Produces: IDR cause token, last host restart cause, receiver backlog depth, skipped-presentation count.

- [ ] **Step 1: Write failing protocol/format tests**

Require host debug round-trip to preserve a restart cause and latency formatting to include `queue=` and `skip_present=`.

- [ ] **Step 2: Run tests to verify they fail**

Run `test_video_feedback`; expected failure is missing fields.

- [ ] **Step 3: Implement cause propagation**

Use compact known tokens: `unknown`, `reassembly-loss`, `decode-failure`, `decode-backlog`, `bitrate-down`, `bitrate-up`, `capture-ended`. Accept legacy `REQUEST_IDR <generation>` as `unknown`; allow `REQUEST_IDR <generation> <cause>` for new peers.

- [ ] **Step 4: Implement receiver queue telemetry**

Update queue depth atomically on push/pop/clear and count decoded frames skipped for presentation. Refresh `decoder_backend` after an auto-hardware fallback succeeds.

- [ ] **Step 5: Run tests to verify they pass**

Run feedback and receiver tests.

- [ ] **Step 6: Commit**

Commit protocol/telemetry changes.

### Task 5: Stop capture churn on bitrate recovery

**Files:**
- Modify: `tests/test_media.cpp`
- Modify: `src/video_sender.cpp`

**Interfaces:**
- Consumes: `target_kbps`, `active_kbps`, `maybe_restart()`.
- Produces: immediate material bitrate downshift restart; no standalone upward-bitrate restart.

- [ ] **Step 1: Write the failing test**

Require sender source to retain the material `target<active` restart path and remove the standalone `target>active` / two-second bitrate-up restart path.

- [ ] **Step 2: Run test to verify it fails**

Run media regression against the old sender; expected failure is the still-present bitrate-up restart condition.

- [ ] **Step 3: Write minimal implementation**

Make `maybe_restart()` restart only for material bitrate downshift or requested IDR. When another legitimate restart happens later, `start_capture()` naturally uses the latest target and recovers bitrate without extra churn.

- [ ] **Step 4: Run test to verify it passes**

Run media regression.

- [ ] **Step 5: Commit**

Commit the restart-policy change.

### Task 6: Audit and verify the low-latency chain

**Files:**
- Modify tests only if an existing optimization lacks a regression assertion.

**Interfaces:**
- Consumes: sender, packetizer/FEC, crypto, receiver batching, decoder, presenter, audio.
- Produces: evidence that the intended optimizations remain present after stabilization.

- [ ] **Step 1:** Verify two-datagram sender burst and 4x IDR pacing.
- [ ] **Step 2:** Verify `VideoFragmentCursor` references encoded access units without per-packet payload vectors and FEC reuses parity storage.
- [ ] **Step 3:** Verify persistent `VideoCipher`, reusable wire/plaintext buffers, replay window, and batched UDP receive.
- [ ] **Step 4:** Verify software fallback after lazy hardware decode failure and telemetry backend refresh.
- [ ] **Step 5:** Verify stride-aware GL plane upload, cached window dimensions, swap interval 0, compositor bypass, and no presenter queue.
- [ ] **Step 6:** Verify bounded audio queue/reset behavior.
- [ ] **Step 7:** Run all locally buildable targeted tests and inspect the final `main` diff.
