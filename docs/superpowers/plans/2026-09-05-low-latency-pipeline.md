# OPAL Low-Latency Pipeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the remaining whole-frame latency from capture, transport, decode, presentation, and conservative 60 FPS defaults without sacrificing bounded behavior or compatibility.

**Architecture:** Add trustworthy stage attribution first, then optimize transport so measurements are meaningful, then introduce native PipeWire capture and zero-copy DRM PRIME presentation behind runtime/build capability gates. Preserve current subprocess and CPU-copy paths as fallbacks.

**Tech Stack:** C++20, Linux UDP/`sendmmsg`, PipeWire/SPA, FFmpeg/libavcodec/libavutil, DRM PRIME, EGL/OpenGL, SDL3, OpenSSL.

**Spec:** `docs/superpowers/specs/2026-09-05-low-latency-pipeline-design.md`

## Global Constraints

- Keep direct encrypted UDP, rendezvous/relay, clipboard, audio, keyboard, mouse, and SDL3 behavior.
- Work on `main`; implementation commits start with `HPI:`.
- No fake local cursor.
- Keep every queue bounded and latest-first.
- Native fast paths are optional; existing subprocess/CPU paths remain fallbacks.
- Do not require GitHub Actions.

---

### Task 1: Make latency telemetry honest

**Files:**
- Modify: `include/opal/media_profile.hpp`
- Modify: `src/media_profile.cpp`
- Modify: `include/opal/video_capture.hpp`
- Modify: `src/video_capture.cpp`
- Modify: `include/opal/video_feedback.hpp`
- Modify: `src/video_feedback.cpp`
- Modify: `src/video_sender.cpp`
- Modify: `src/video_receiver.cpp`
- Modify: `src/video_present.cpp`
- Test: `tests/test_media_profile.cpp`
- Test: `tests/test_video_capture.cpp`
- Test: `tests/test_video_feedback.cpp`

**Interfaces:**
- Produces `CaptureTimestampQuality { Estimated, Exact }` and debug fields for acquisition quality, capture-to-encoded/packet, upload/import, and swap-submit timing.
- Existing debug line parsers remain backward-compatible when new optional fields are absent.

- [ ] **Step 1: Add failing timestamp-quality tests**

```cpp
static_assert(opal::capture_timestamp_quality_name(opal::CaptureTimestampQuality::Estimated)==std::string_view("estimated"));
static_assert(opal::capture_timestamp_quality_name(opal::CaptureTimestampQuality::Exact)==std::string_view("exact"));
```

- [ ] **Step 2: Verify RED**

Run: `make test-media-profile test-video-feedback`
Expected: compile failure because the new enum/helpers/debug fields do not exist.

- [ ] **Step 3: Implement minimal telemetry fields and labels**

```cpp
enum class CaptureTimestampQuality : std::uint8_t { Estimated=0, Exact=1 };
std::string_view capture_timestamp_quality_name(CaptureTimestampQuality);
```

`VideoCapture` exposes `capture_timestamp_quality()`; subprocess capture returns `Estimated`. Sender debug exports the quality plus existing capture-to-packet estimate. Presenter reports swap timing as submit/return timing and whether immediate swap activation succeeded.

- [ ] **Step 4: Verify GREEN**

Run: `make test-media-profile test-video-capture test-video-feedback test-video-present`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git commit -am "HPI: make latency attribution explicit"
```

### Task 2: Remove deliberate LAN frame spreading and use batched sends

**Files:**
- Modify: `include/opal/udp_transport.hpp`
- Modify: `src/udp_transport.cpp`
- Modify: `include/opal/video_sender.hpp`
- Modify: `src/video_sender.cpp`
- Modify: `src/peer_session.cpp`
- Test: `tests/test_udp_transport.cpp`
- Test: `tests/test_media_profile.cpp`
- Test: `tests/test_direct_video_pipeline.cpp`

**Interfaces:**
- Add a bounded batch-send callback for media frames while keeping the existing single-datagram callback for compatibility/tests.
- Direct/LAN media can burst one encoded frame within a bounded byte/datagram cap; relay/congested paths retain pacing.

- [ ] **Step 1: Add failing batch-send behavior tests**

```cpp
std::array<std::span<const std::uint8_t>,3> batch{a,b,c};
auto r=opal::send_datagrams_batch(fd,peer,peer_len,batch);
assert(r.sent<=3);
assert(r.result==opal::UdpSendResult::Sent || r.result==opal::UdpSendResult::WouldBlock);
```

Add a sender policy assertion that low-latency direct mode has a full-frame bounded burst budget while paced mode retains the current 1.2x rate.

- [ ] **Step 2: Verify RED**

Run: `make test-udp-transport test-media-profile`
Expected: FAIL on missing policy/batch integration behavior.

- [ ] **Step 3: Implement bounded production batching**

Packetize/encrypt into reusable frame-local storage capped by the existing maximum frame/fragment constraints, then submit up to `kUdpSendBatchMax` datagrams per `sendmmsg()` call. A partial batch advances only by the returned count. `WouldBlock` drops the obsolete remainder of an ordinary frame; keyframes/config retain bounded retry behavior.

- [ ] **Step 4: Verify GREEN**

Run: `make test-udp-transport test-media-profile test-direct-video-pipeline test-direct-video-stress`
Expected: PASS and sender debug `send=` span falls materially on clean local tests.

- [ ] **Step 5: Commit**

```bash
git commit -am "HPI: batch direct media frames for lower latency"
```

### Task 3: Add optional native PipeWire capture with exact timestamps

**Files:**
- Create: `include/opal/pipewire_capture.hpp`
- Create: `src/pipewire_capture.cpp`
- Create: `include/opal/native_encoder.hpp`
- Create: `src/native_encoder.cpp`
- Modify: `include/opal/video_capture.hpp`
- Modify: `src/video_capture.cpp`
- Modify: `Makefile.core`
- Test: `tests/test_pipewire_capture.cpp`
- Test: `tests/test_video_capture.cpp`
- Test: `tests/test_build_flags.cpp` or `test-build-flags` recipe

**Interfaces:**
- `PipeWireCapture::start(StreamOptions)` negotiates raw video and delivers bounded latest frames with monotonic acquisition timestamps.
- `NativeEncoder` owns one FFmpeg H.264 encoder context for the session and emits H.264 access units/config without FLV.
- `VideoCapture` prefers native backend when compiled and usable; otherwise it falls back to existing GSR/FFmpeg FLV subprocess capture.

- [ ] **Step 1: Add failing backend-selection tests**

```cpp
assert(opal::native_capture_compiled()==expected_from_build);
assert(opal::VideoCapture::backend_preference().front()=="pipewire-native");
```

Unit tests use a fake frame source interface; they must not require a live desktop portal.

- [ ] **Step 2: Verify RED**

Run: `make test-video-capture test-build-flags`
Expected: FAIL because native capture interfaces/build flags do not exist.

- [ ] **Step 3: Add optional PipeWire build detection**

Use `pkg-config --exists libpipewire-0.3` and compile `src/pipewire_capture.cpp` with `-DOPAL_HAVE_PIPEWIRE=1` only when available. Do not make PipeWire headers mandatory for baseline builds.

- [ ] **Step 4: Implement capture/encoder fast path**

Use `pw_stream` input with `PW_STREAM_FLAG_AUTOCONNECT|PW_STREAM_FLAG_MAP_BUFFERS`, negotiate common raw video formats and `SPA_DATA_DmaBuf` where available, take the timestamp when a `pw_buffer` is dequeued, and immediately queue/recycle buffers after the encoder has consumed/imported them. Keep one FFmpeg encoder context, `AV_CODEC_FLAG_LOW_DELAY`, no B-frames, small VBV, and hardware encoder preference where supported.

- [ ] **Step 5: Verify GREEN**

Run: `make test-video-capture test-build-flags test-media test-direct-video-pipeline`
Expected: PASS with fallback on headless environments.

- [ ] **Step 6: Commit**

```bash
git add Makefile.core include/opal/pipewire_capture.hpp include/opal/native_encoder.hpp src/pipewire_capture.cpp src/native_encoder.cpp include/opal/video_capture.hpp src/video_capture.cpp tests/test_pipewire_capture.cpp tests/test_video_capture.cpp
git commit -m "HPI: add native PipeWire capture and persistent encoder"
```

### Task 4: Preserve DRM PRIME frames and import them through EGL

**Files:**
- Modify: `include/opal/video_decoder.hpp`
- Modify: `src/video_decoder.cpp`
- Modify: `include/opal/video_present.hpp`
- Modify: `src/video_present.cpp`
- Modify: `Makefile.core`
- Test: `tests/test_video_decoder.cpp`
- Test: `tests/test_video_present.cpp`

**Interfaces:**
- Decoder output may contain `AV_PIX_FMT_DRM_PRIME` and retain the owning `AVFrame` reference.
- Presenter first attempts DRM PRIME/EGL import and falls back to existing YUV420/NV12 CPU upload.

- [ ] **Step 1: Add failing capability/fallback tests**

```cpp
assert(opal::VideoPresenter::supports_cpu_upload_format(AV_PIX_FMT_YUV420P));
assert(opal::VideoPresenter::drm_prime_supported_format(AV_PIX_FMT_DRM_PRIME));
```

Decoder test asserts an explicit `OPAL_DECODER=software` path still returns software frames unchanged.

- [ ] **Step 2: Verify RED**

Run: `make test-video-decoder test-video-present`
Expected: FAIL on missing DRM PRIME capability API.

- [ ] **Step 3: Preserve hardware frames**

If FFmpeg returns `AV_PIX_FMT_DRM_PRIME`, move/reference that frame to `latest` instead of calling `av_hwframe_transfer_data`. Keep CPU transfer for other hardware formats that cannot be imported.

- [ ] **Step 4: Implement EGL DMA-BUF import**

Read `AVDRMFrameDescriptor` from `frame->data[0]`, build `EGL_LINUX_DMA_BUF_EXT` attributes from object fd, layer format, plane offset/pitch, and modifier when supported, create `EGLImageKHR`, bind it to GL texture(s), render, then destroy the image after swap while the `AVFrame` still owns the fds.

- [ ] **Step 5: Verify GREEN**

Run: `make test-video-decoder test-video-present test-direct-video-pipeline`
Expected: PASS. Headless/no-EGL-import systems exercise CPU fallback.

- [ ] **Step 6: Commit**

```bash
git commit -am "HPI: add DRM PRIME zero-copy presentation"
```

### Task 5: Adaptive high-FPS defaults and final gates

**Files:**
- Modify: `include/opal/media_profile.hpp`
- Modify: `src/media_profile.cpp`
- Modify: `src/client.cpp`
- Modify: `src/main.cpp`
- Test: `tests/test_media_profile.cpp`
- Test: `tests/test_core.cpp`

**Interfaces:**
- `automatic_stream_fps(display_refresh_hz, capture_max_fps)` selects from 60/90/120/144/165/240 and never exceeds either capability.
- Explicit `--fps` always wins.

- [ ] **Step 1: Add failing FPS selector tests**

```cpp
assert(opal::automatic_stream_fps(60,240)==60);
assert(opal::automatic_stream_fps(144,240)==144);
assert(opal::automatic_stream_fps(165,120)==120);
assert(opal::automatic_stream_fps(0,0)==60);
```

- [ ] **Step 2: Verify RED**

Run: `make test-media-profile test-core`
Expected: FAIL because selector is missing.

- [ ] **Step 3: Implement selector and client display query**

SDL display-mode refresh is used when available before connection. Default stream FPS becomes the selected latency-oriented rate; explicit `--fps` remains unchanged.

- [ ] **Step 4: Verify focused suite**

Run: `make test-media-profile test-core test-video-capture test-video-decoder test-video-present test-udp-transport test-direct-video-pipeline test-direct-video-stress`
Expected: PASS.

- [ ] **Step 5: Run release gates on Fedora integration host**

Run:

```bash
make test-hpi
make test-hpi-sanitize
make test-netem
OPAL_SOAK_SECONDS=60 make test-soak
```

Expected: PASS, except the already-proven Fedora/Asahi `libtsan` condition-variable runtime skip.

- [ ] **Step 6: Commit**

```bash
git commit -am "HPI: select higher refresh rates for latency"
```
