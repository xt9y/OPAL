# OPAL Direct-UDP Ultra-Low-Latency Video Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace OPAL's relayed ordered video stream with a latency-first 1080p60 direct encrypted UDP media path that discards stale work instead of buffering behind real time, while preserving reliable TLS/zrok control and input.

**Architecture:** Pairing, authentication, keyboard, mouse, Wake-on-LAN, heartbeat, and control recovery stay on the existing TLS 1.3 connection carried by a zrok private TCP share. A separate media stack discovers a direct UDP path with local/STUN candidates, derives per-generation ChaCha20-Poly1305 keys from the authenticated TLS session, packetizes encoded H.264/AAC into MTU-safe frame-aware datagrams, performs bounded XOR FEC/reassembly, decodes with libavcodec, and presents only the newest useful frame through an OPAL-owned X11/GLX surface. Tasks 3-10 build and prove this path behind explicit development/test wiring; Task 11 is the single production cutover and simultaneously removes zrok-video/TCP-video/ffplay so there is never a half-migrated production fallback policy.

**Tech Stack:** C++20, OpenSSL 3/TLS 1.3, POSIX UDP sockets, STUN/RFC 8489 binding discovery, GPU Screen Recorder, FFmpeg/libavformat/libavcodec/libavutil/libswresample, X11/XInput2, OpenGL/GLX, PulseAudio, Make, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-09-03-direct-udp-ultra-low-latency-video-design.md`

## Global Constraints

- Work directly on `main`, as requested for OPAL development.
- Plain `opal` defaults to a 1920x1080 ceiling at 60 FPS; a smaller host is never upscaled.
- `--mode max|1080p|1440p|4k` and `--fps 15-240` remain temporary per-connection overrides and are never persisted.
- After Task 11, direct encrypted UDP is the only production media transport: no TURN, no zrok UDP media, no TCP video fallback.
- Tasks 3-10 may expose the new path only through focused tests or `OPAL_DIRECT_VIDEO_DEV=1`; normal `opal` remains on the old path until the Task 11 atomic cutover.
- Direct UDP negotiation has one absolute 5,000 ms deadline after authenticated control setup; STUN/candidate/probe phases do not reset that deadline.
- Maximum OPAL UDP datagram size is 1,200 bytes total, including header, ciphertext, and 16-byte AEAD tag.
- Media keys are direction-specific and derived fresh from the exact authenticated TLS control generation. A control recovery invalidates old UDP keys/session ids immediately.
- Replay protection is a fixed 1,024-packet window per direction/generation.
- Default IDR cadence is 250 ms; at 60 FPS this is approximately 15 frames.
- Sender retains at most 2 encoded video access units and 16 MiB total encoded queue state, whichever limit is reached first.
- Receiver retains at most 3 in-progress video access units and 16 MiB total reassembly state, whichever limit is reached first.
- FEC is one XOR parity fragment per group of up to 10 ordinary fragments; it never waits into a later frame.
- Ordinary stale P-frame fragments are never retransmitted.
- Presentation keeps at most one unpublished decoded frame and has no playback/jitter queue.
- Audio never gates video; native audio buffering is capped at 40 ms and stale audio is discarded.
- `VIDEO_FEEDBACK` is emitted at most once every 100 ms and never blocks media delivery.
- A failed direct-only negotiation must become a clear user-visible error after Task 11, not a reconnect loop or relay fallback.
- TDD for each production task: write focused failing regression, prove RED, implement minimal behavior, prove GREEN, commit.
- Because work is directly on `main`, an intentionally failing RED test commit may briefly make CI red; the immediately following implementation commit must restore the affected gates before advancing.
- Final completion requires Linux, ASan/UBSan, TSan, and stress jobs green on the exact final `main` SHA.

## File Structure

- `include/opal/media_profile.hpp`, `src/media_profile.cpp`: default stream options, resolution-mode mapping, bitrate ceiling.
- `include/opal/video_capture.hpp`, `src/video_capture.cpp`: capture subprocess plus libavformat demux into encoded media units.
- `include/opal/udp_transport.hpp`, `src/udp_transport.cpp`: UDP socket lifecycle, interface candidates, STUN, datagram I/O/address parsing.
- `include/opal/video_crypto.hpp`, `src/video_crypto.cpp`: TLS exporter, ChaCha20-Poly1305, nonce construction, replay window.
- `include/opal/video_packet.hpp`, `src/video_packet.cpp`: wire header, serialization, fragmentation, XOR parity generation.
- `include/opal/video_reassembly.hpp`, `src/video_reassembly.cpp`: bounded fragment/FEC/frame freshness state.
- `include/opal/direct_video_session.hpp`, `src/direct_video_session.cpp`: synchronous authenticated candidate exchange and hole punching per control generation.
- `include/opal/video_decoder.hpp`, `src/video_decoder.cpp`: low-delay H.264 and AAC decoder primitives.
- `include/opal/video_present.hpp`, `src/video_present.cpp`: fullscreen X11/GLX newest-frame renderer.
- `include/opal/audio_output.hpp`, `src/audio_output.cpp`: bounded PulseAudio playback.
- `include/opal/video_feedback.hpp`, `src/video_feedback.cpp`: pacing, congestion feedback, clock offset, latency telemetry.
- `include/opal/video_sender.hpp`, `src/video_sender.cpp`: host capture queue, packetization/encryption/FEC/pacing/send and IDR recovery.
- `include/opal/video_receiver.hpp`, `src/video_receiver.cpp`: client receive/auth/reassembly/decode/present/audio pipeline.
- `src/session.cpp`: client control-generation orchestration only.
- `src/host.cpp`: authenticated host control/input orchestration plus ownership of one media sender for the active control generation.
- `src/tunnel.cpp`, `src/tunnel_access.cpp`: final control-only zrok lifecycle; legacy video tokens survive only for migration/cleanup.

---

### Task 1: Default plain OPAL to 1080p60 and extract media-profile policy

**Files:** Create `include/opal/media_profile.hpp`, `src/media_profile.cpp`, `tests/test_media_profile.cpp`; modify `include/opal/media.hpp`, `src/media.cpp`, `include/opal/client.hpp`, `include/opal/setup.hpp`, `include/opal/session.hpp`, `src/main.cpp`, `tests/test_setup.cpp`, `tests/smoke.sh`, `Makefile`.

**Interface:**

```cpp
namespace opal {
struct StreamOptions {
    int max_width=1920;
    int max_height=1080;
    int fps=60;
};
StreamOptions default_stream_options();
bool stream_mode_limit(const std::string&,int&,int&);
int automatic_bitrate_kbps(int width,int height,int fps);
}
```

- [ ] **Step 1 — RED:** create `test_media_profile.cpp` and assert default `{1920,1080,60}`, explicit `max` -> `{0,0}`, 1080p/1440p/4k mappings, invalid-mode rejection, and 1080p60 automatic bitrate 30000 kbps. Extend setup tests so plain `interactive_run()` forwards 1080p60 while `{0,0,60}` remains a temporary native override; assert no stream keys are saved. Require 1080p60 wording in smoke help.
- [ ] **Step 2 — Prove RED:** `make test-media-profile test-setup`; expected compile/stale-default failure.
- [ ] **Step 3 — GREEN:** move `StreamOptions`, mode mapping, and bitrate policy out of `media.*`; implement `StreamOptions default_stream_options(){ return {}; }`; update help line to `Wake and connect at up to 1080p / 60 FPS`.
- [ ] **Step 4 — Prove GREEN:** `make clean test-media-profile test-media test-setup all` plus `BIN="$PWD/build/opal" INPUT_BIN="$PWD/build/opal-input" ./tests/smoke.sh`.
- [ ] **Step 5 — Commit:** `git commit -m "feat: default OPAL streaming to 1080p60"`.

---

### Task 2: Demux capture into encoded H.264/AAC units and add native-media build dependencies

**Files:** Create `include/opal/video_capture.hpp`, `src/video_capture.cpp`, `tests/test_video_capture.cpp`; modify `src/media.cpp`, `Makefile`, `.github/workflows/ci.yml`, `src/system.cpp`, `README.md`.

**Interface:**

```cpp
enum class MediaKind { VideoH264, AudioAac };
struct MediaConfig {
    MediaKind kind;
    std::vector<std::uint8_t> extradata;
    int sample_rate=0;
    int channels=0;
};
struct EncodedMediaUnit {
    MediaKind kind;
    std::vector<std::uint8_t> data;
    std::int64_t pts_us=0;
    bool keyframe=false;
};
class VideoCapture {
public:
    bool start(const StreamOptions&,int bitrate_kbps,bool audio,const std::string& portal_token_file);
    bool next(EncodedMediaUnit&,int timeout_ms);
    const std::vector<MediaConfig>& configs() const;
    void stop();
    ~VideoCapture();
};
```

- [ ] **Step 1 — RED:** use `OPAL_CAPTURE_CMD` with a bounded real FLV fixture:

```bash
ffmpeg -hide_banner -loglevel error -re \
 -f lavfi -i testsrc=size=320x180:rate=60 \
 -f lavfi -i sine=frequency=440:sample_rate=48000 \
 -c:v libx264 -preset ultrafast -tune zerolatency -bf 0 -g 15 -keyint_min 15 -sc_threshold 0 \
 -c:a aac -b:a 96k -f flv pipe:1
```

Assert `VideoCapture` yields H.264, AAC, codec configs and a keyframe within five seconds. In media tests require GSR `-keyint 0.25`, H.264 CBR and `-tune performance`; require FFmpeg fallback GOP `max(1,fps/4)` and `-bf 0`.
- [ ] **Step 2 — Prove RED:** `make test-video-capture`; expected missing interface.
- [ ] **Step 3 — GREEN:** add `pkg-config` build flags for `libavformat libavcodec libavutil libswresample` and `-lGL`; add Ubuntu CI packages `pkg-config libavcodec-dev libavformat-dev libavutil-dev libswresample-dev libgl1-mesa-dev libpulse-dev xvfb`. Implement custom `AVIOContext` over the capture fd, open FLV with libavformat, copy codec extradata, convert packet PTS to microseconds, and emit packet payloads rather than container bytes. Set GSR key interval 0.25 s and fallback GOP `fps/4`.
- [ ] **Step 4 — Prove GREEN:** `make clean test-video-capture test-media test-core all`.
- [ ] **Step 5 — Commit:** `git commit -m "feat: expose encoded media units"`.

---

### Task 3: Implement direct UDP sockets and STUN candidate discovery

**Files:** Create `include/opal/udp_transport.hpp`, `src/udp_transport.cpp`, `tests/test_udp_transport.cpp`; modify `Makefile`.

**Interface:**

```cpp
enum class CandidateType { Local, ServerReflexive };
struct StunEndpoint { std::string host; std::uint16_t port; };
struct UdpCandidate { std::string host; std::uint16_t port; CandidateType type; };
struct UdpSocket { int fd=-1; std::uint16_t local_port=0; };
UdpSocket open_udp_socket();
void close_udp_socket(UdpSocket&);
std::vector<UdpCandidate> local_udp_candidates(const UdpSocket&);
std::vector<StunEndpoint> default_stun_endpoints();
std::optional<UdpCandidate> discover_server_reflexive_candidate(
    const UdpSocket&,const std::vector<StunEndpoint>&,int timeout_ms);
bool send_datagram(int,const sockaddr_storage&,socklen_t,std::span<const std::uint8_t>);
int recv_datagram(int,std::span<std::uint8_t>,sockaddr_storage&,socklen_t&,int timeout_ms);
```

Built-ins: `stun.cloudflare.com:3478` and `stunserver2025.stunprotocol.org:3478`; tests use a local fake STUN server only.

- [ ] **Step 1 — RED:** test bidirectional loopback UDP; fake RFC 8489 Binding Response with matching transaction id/XOR-MAPPED-ADDRESS; reject wrong transaction id, malformed attribute lengths and oversized packets; verify supplied timeout is bounded.
- [ ] **Step 2 — Prove RED:** `make test-udp-transport`.
- [ ] **Step 3 — GREEN:** create CLOEXEC nonblocking UDP socket bound to ephemeral port; enumerate usable AF_INET/AF_INET6 interface addresses; implement 20-byte STUN Binding Request with magic `0x2112A442` and random 96-bit transaction id; parse matching XOR-MAPPED-ADDRESS with four-byte attribute alignment. STUN is routing discovery only, never authentication.
- [ ] **Step 4 — Prove GREEN:** `make clean test-udp-transport test-net`.
- [ ] **Step 5 — Commit:** `git commit -m "feat: add direct UDP candidate discovery"`.

---

### Task 4: Derive TLS-bound media keys, ChaCha20-Poly1305 AEAD, and replay protection

**Files:** Create `include/opal/video_crypto.hpp`, `src/video_crypto.cpp`, `tests/test_video_crypto.cpp`; modify `Makefile`.

**Interface:**

```cpp
struct VideoKeys {
    std::array<std::uint8_t,32> send_key{},recv_key{};
    std::array<std::uint8_t,12> send_nonce_base{},recv_nonce_base{};
};
bool derive_video_keys(SSL*,std::string_view session_token,
    std::string_view client_pub,std::string_view host_fp,bool client_side,VideoKeys&);
bool seal_video_datagram(const VideoKeys&,std::uint64_t seq,
    std::span<const std::uint8_t> aad,std::span<const std::uint8_t> plaintext,
    std::vector<std::uint8_t>& ciphertext_tag);
bool open_video_datagram(const VideoKeys&,std::uint64_t seq,
    std::span<const std::uint8_t> aad,std::span<const std::uint8_t> ciphertext_tag,
    std::vector<std::uint8_t>& plaintext);
class ReplayWindow1024 { public: bool accept(std::uint64_t); void reset(); };
```

- [ ] **Step 1 — RED:** localhost TLS fixture derives client/server keys; assert client send==server recv and vice versa; 1000-byte AEAD roundtrip succeeds; ciphertext/AAD tampering fails; replay duplicate and sequence >1024 behind highest are rejected.
- [ ] **Step 2 — Prove RED:** `make test-video-crypto`.
- [ ] **Step 3 — GREEN:** call `SSL_export_keying_material` for 88 bytes with label `EXPORTER-OPAL-DIRECT-VIDEO-v1` and context `session_token + "\n" + client_pub + "\n" + host_fp`; map two 32-byte keys and two 12-byte nonce bases, swapping directions on client. Nonce = base with big-endian packet sequence XORed into final eight bytes. Use `EVP_chacha20_poly1305()` and 16-byte tag. Replay window is a fixed 1024-bit sliding bitmap.
- [ ] **Step 4 — Prove GREEN:** `make clean test-video-crypto test-net test-hardening`.
- [ ] **Step 5 — Commit:** `git commit -m "feat: authenticate direct video datagrams"`.

---

### Task 5: Define the 1200-byte frame packet, fragmentation, XOR FEC, and bounded reassembly

**Files:** Create `include/opal/video_packet.hpp`, `src/video_packet.cpp`, `include/opal/video_reassembly.hpp`, `src/video_reassembly.cpp`, `tests/test_video_packet.cpp`, `tests/test_video_reassembly.cpp`; modify `Makefile`.

**Wire format:** fixed 52-byte authenticated header before ciphertext/tag:

```cpp
enum class VideoMediaType : std::uint8_t { VideoH264=1,AudioAac=2,Probe=3,ProbeAck=4,Fec=5 };
enum VideoFrameFlags : std::uint16_t { FrameKeyframe=1u,FrameConfig=2u,FrameEnd=4u };
struct VideoPacketHeader {
    std::uint32_t magic=0x4f505631; // OPV1
    std::uint8_t version=1;
    VideoMediaType media_type=VideoMediaType::VideoH264;
    std::uint16_t flags=0;
    std::uint32_t generation=0;
    std::uint64_t session_id=0,packet_sequence=0,frame_id=0,capture_timestamp_us=0;
    std::uint16_t fragment_index=0,fragment_count=0,fec_group=0,payload_length=0;
};
```

Maximum plaintext media fragment = `1200 - 52 - 16 = 1132` bytes.

- [ ] **Step 1 — RED:** network-byte-order roundtrip; wrong magic/version/length rejection; fragment 100 KiB H.264 into <=1200-byte datagrams; parity one per <=10 data fragments; one missing fragment recovered; two missing in one group not recovered; reassembled bytes exact; fourth in-progress frame evicts oldest stale ordinary frame; allocation never exceeds 16 MiB; generation reset drops old state.
- [ ] **Step 2 — Prove RED:** `make test-video-packet test-video-reassembly`.
- [ ] **Step 3 — GREEN:** serialize fields explicitly, never reinterpret unaligned network memory as the struct. FEC plaintext stores `uint8 count`, ten `uint16 original_length` slots, then XOR bytes up to longest fragment. `VideoReassembler` enforces 3-frame/16-MiB bounds before allocation and reports `NeedIdr` when an unrecoverable reference loss means later P-frames are unusable.
- [ ] **Step 4 — Prove GREEN:** `make clean test-video-packet test-video-reassembly` under normal and sanitizer builds.
- [ ] **Step 5 — Commit:** `git commit -m "feat: add frame-aware video datagrams"`.

---

### Task 6: Negotiate one authenticated direct path synchronously on each control generation

**Files:** Create `include/opal/direct_video_session.hpp`, `src/direct_video_session.cpp`, `tests/test_direct_video_session.cpp`; modify `include/opal/session.hpp`, `src/session.cpp`, `src/host.cpp`, `Makefile`.

**Interface:**

```cpp
struct DirectVideoPath {
    UdpSocket socket;
    sockaddr_storage peer{};
    socklen_t peer_len=0;
    VideoKeys keys;
    std::uint64_t session_id=0;
    std::uint32_t generation=0;
};
using ControlSend=std::function<bool(const std::string&,int)>;
using ControlRead=std::function<bool(std::string&,int)>;
bool negotiate_client_direct_video(SSL*,const std::string& session_token,
    const std::string& client_pub,const std::string& host_fp,std::uint32_t generation,
    const std::vector<StunEndpoint>&,ControlSend,ControlRead,DirectVideoPath&,std::string&,int deadline_ms=5000);
bool negotiate_host_direct_video(SSL*,const std::string& session_token,
    const std::string& client_pub,const std::string& host_fp,std::uint32_t generation,
    const std::vector<StunEndpoint>&,ControlSend,ControlRead,DirectVideoPath&,std::string&,int deadline_ms=5000);
```

Control grammar:

```text
UDP_CANDIDATE <generation> <L|S> <host> <port>
UDP_CANDIDATES_DONE <generation>
UDP_PROBE_READY <generation>
UDP_SELECTED <generation> <host> <port>
REQUEST_IDR <generation>
```

- [ ] **Step 1 — RED:** localhost TLS fixture runs client/host negotiators concurrently; local candidates select within one second; forged probe with invalid tag cannot select; unreachable candidates with `deadline_ms=250` fail within bound and return exact direct-only error; reject >16 candidates, bad generation/port, host strings >255 bytes and extra fields.
- [ ] **Step 2 — Prove RED:** `make test-direct-video-session`.
- [ ] **Step 3 — GREEN:** immediately after authenticated `OK <session-token>` and before heartbeat/control-reader threads start, both sides gather/exchange candidates synchronously on the sole TLS reader. After `*_DONE`, derive keys and send authenticated `Probe` packets every 50 ms to candidate addresses; first valid `ProbeAck` source wins. Use one absolute deadline. After selection, return TLS reading to the existing serialized control loop. At this task, invoke negotiation only from focused tests or when `OPAL_DIRECT_VIDEO_DEV=1`; normal media remains old path.
- [ ] **Step 4 — Prove GREEN:** `make clean test-direct-video-session test-session test-net test-udp-transport test-video-crypto`.
- [ ] **Step 5 — Commit:** `git commit -m "feat: negotiate authenticated direct UDP video"`.

---

### Task 7: Add low-delay H.264 decode and OPAL-owned X11/GLX newest-frame presentation

**Files:** Create `include/opal/video_decoder.hpp`, `src/video_decoder.cpp`, `include/opal/video_present.hpp`, `src/video_present.cpp`, `tests/test_video_decoder.cpp`, `tests/test_video_present.cpp`; modify `Makefile`, `.github/workflows/ci.yml`.

**Interface:**

```cpp
struct DecodedVideoFrame { AVFrame *frame=nullptr; std::int64_t pts_us=0; };
class VideoDecoder {
public:
    bool configure_h264(std::span<const std::uint8_t> extradata);
    bool decode(std::span<const std::uint8_t> unit,std::int64_t pts_us,
                std::vector<DecodedVideoFrame>& out);
    void flush();
    ~VideoDecoder();
};
class VideoPresenter {
public:
    bool open(int source_width,int source_height,bool fullscreen=true);
    bool present(DecodedVideoFrame frame); // consumes frame ownership
    Window x11_window() const;
    std::pair<int,int> drawable_size() const;
    std::size_t pending_frame_count() const; // always 0 or 1
    void close();
    ~VideoPresenter();
};
```

- [ ] **Step 1 — RED:** generate four 320x180 H.264 frames with libx264 ultrafast/zerolatency/no-B; verify decoder emits without waiting for future frames. Under `Xvfb :99`, open a 320x180 non-fullscreen presenter and submit three frames faster than display; assert pending count never exceeds one and replaced frames are freed.
- [ ] **Step 2 — Prove RED:** `make test-video-decoder test-video-present`.
- [ ] **Step 3 — GREEN:** decoder uses `AV_CODEC_FLAG_LOW_DELAY`, `AV_CODEC_FLAG2_FAST`, `thread_count=1`, `thread_type=0`, `avcodec_send_packet/receive_frame`. Presenter creates X11 window and one GLX context on current `DISPLAY`, uploads YUV420P Y/U/V textures (NV12 Y+UV branch), converts in fragment shader, and swaps immediately with no app playback clock. Unexpected formats may use a bounded conversion path; no steady-state CPU RGB conversion.
- [ ] **Step 4 — Prove GREEN:** start Xvfb and run `DISPLAY=:99 make clean test-video-decoder test-video-present`.
- [ ] **Step 5 — Commit:** `git commit -m "feat: add native low-delay video playback"`.

---

### Task 8: Build the real direct H.264 sender/receiver behind development wiring

**Files:** Create `include/opal/video_sender.hpp`, `src/video_sender.cpp`, `include/opal/video_receiver.hpp`, `src/video_receiver.cpp`, `tests/test_direct_video_pipeline.cpp`; modify `src/host.cpp`, `src/session.cpp`, `Makefile`.

**Interface:**

```cpp
class VideoSender {
public:
    bool start(DirectVideoPath,const StreamOptions&,bool audio,
               std::function<void(const std::string&)> control_send);
    void request_idr();
    void stop();
};
class VideoReceiver {
public:
    bool start(DirectVideoPath,std::function<void(const std::string&)> control_send);
    bool media_started() const;
    Window presentation_window() const;
    void stop();
};
```

- [ ] **Step 1 — RED:** loopback direct path + real synthetic capture; assert decoded frame presented within five seconds, every emitted datagram <=1200 bytes, and no ffplay process is involved in the development path. Loss shim drops one fragment -> FEC recovers without IDR; drops two reference fragments -> receiver emits `REQUEST_IDR`, sender recovers and presents again. Block send capacity and prove queue remains <=2 frames/16 MiB and stale P-frame is discarded.
- [ ] **Step 2 — Prove RED:** `DISPLAY=:99 make test-direct-video-pipeline`.
- [ ] **Step 3 — GREEN:** sender owns `VideoCapture`, emits config/keyframe flags, packetizes/FEC/encrypts/nonblocking-sends. `EAGAIN`/queue pressure drops oldest ordinary P-frame. Receiver authenticates/replay-checks/reassembles/decodes/presents newest useful frame. `REQUEST_IDR` sets a sender flag; first portable implementation restarts `VideoCapture` at current profile/bitrate, rate-limited to once/250 ms, yielding fresh config+IDR. Wire this only under `OPAL_DIRECT_VIDEO_DEV=1` and tests. Plain production `opal` still uses current media until Task 11.
- [ ] **Step 4 — Prove GREEN:** `DISPLAY=:99 make clean test-direct-video-pipeline test-direct-video-session test-video-packet test-video-reassembly test-video-decoder test-video-present test-session`.
- [ ] **Step 5 — Commit:** `git commit -m "feat: stream OPAL video over direct UDP"`.

---

### Task 9: Add <=40 ms AAC/PulseAudio output without letting audio gate video

**Files:** Create `include/opal/audio_output.hpp`, `src/audio_output.cpp`, `tests/test_audio_output.cpp`; modify `src/video_receiver.cpp`, `Makefile`, `.github/workflows/ci.yml`.

**Interface:**

```cpp
class AudioOutput {
public:
    bool configure_aac(std::span<const std::uint8_t> extradata,int sample_rate,int channels);
    bool submit(std::span<const std::uint8_t> aac,std::int64_t pts_us,
                std::int64_t current_video_pts_us);
    void reset_to(std::int64_t video_pts_us);
    std::uint32_t queued_ms() const;
    void close();
};
```

- [ ] **Step 1 — RED:** injectable test sink decodes short AAC fixture; queue never >40 ms; packet older than `current_video_pts_us-40000` discarded; `reset_to` immediately drops stale samples; video test remains successful with sink unavailable.
- [ ] **Step 2 — Prove RED:** `make test-audio-output`.
- [ ] **Step 3 — GREEN:** libavcodec AAC decode + libswresample to signed 16-bit interleaved PCM. Use asynchronous libpulse playback with `maxlength/tlength` equal to 40 ms PCM, `minreq` about 10 ms, `prebuf=0`. Drop stale audio before decode/write and never wait to present video for audio synchronization. Feed reassembled `AudioAac` units independently in receiver development path.
- [ ] **Step 4 — Prove GREEN:** `make clean test-audio-output test-direct-video-pipeline`.
- [ ] **Step 5 — Commit:** `git commit -m "feat: add latency-bounded direct audio"`.

---

### Task 10: Add pacing, 100 ms feedback, adaptive bitrate, clock sync, and latency telemetry

**Files:** Create `include/opal/video_feedback.hpp`, `src/video_feedback.cpp`, `tests/test_video_feedback.cpp`; modify `src/video_sender.cpp`, `src/video_receiver.cpp`, `src/session.cpp`, `src/host.cpp`, `Makefile`.

**Interface:**

```cpp
struct VideoFeedbackSample {
    std::uint64_t highest_sequence=0;
    std::uint32_t received=0,lost=0,rtt_us=0,decode_age_us=0;
};
class BitrateController {
public:
    explicit BitrateController(int ceiling_kbps);
    int on_feedback(const VideoFeedbackSample&,std::chrono::steady_clock::time_point);
    int target_kbps() const;
};
struct LatencyTelemetry {
    double capture_to_packet_ms=0,network_ms=0,reassembly_ms=0,
           decode_ms=0,present_ms=0,total_ms=0,loss_percent=0;
    std::uint64_t stale_frames=0;
    int bitrate_kbps=0;
};
```

Control grammar:

```text
VIDEO_FEEDBACK <generation> <highest_seq> <received> <lost> <rtt_us> <decode_age_us>
CLOCK_SYNC <generation> <t0_us> <t1_us> <t2_us>
```

- [ ] **Step 1 — RED:** exact adaptation tests: initial target=automatic ceiling; loss>2% or RTT inflation>15ms for two consecutive samples -> multiply target by 0.80; floor=`max(4000,ceiling*35/100)`; loss<0.2% and RTT inflation<3ms for >=1s -> multiply by 1.05; never exceed ceiling. Capture reconfigure only if target differs >=15% and last restart >=2s. Reject malformed/wrong-generation feedback. Synthetic clock fixture estimates one-way latency within 1 ms.
- [ ] **Step 2 — Prove RED:** `make test-video-feedback`.
- [ ] **Step 3 — GREEN:** token-bucket pacing at target bitrate with maximum two-datagram burst; if pacing would preserve an ordinary frame beyond sender bounds, drop frame instead of sleeping behind live edge. Receiver sends feedback <=10 Hz. Bitrate controller lowers rapidly and raises conservatively. >=15% target change after two-second cooldown restarts capture with new CBR and fresh IDR. Maintain EWMA alpha 0.2 and print one `OPAL_DEBUG=1` report/sec containing capture->packet, network, reassembly, decode, present, total, loss, stale drops and bitrate. Keep entire feature under development media wiring until Task 11.
- [ ] **Step 4 — Prove GREEN:** `make clean test-video-feedback test-direct-video-pipeline test-session`.
- [ ] **Step 5 — Commit:** `git commit -m "perf: adapt direct video for minimum latency"`.

---

### Task 11: Atomically cut production over to direct media and delete relayed/TCP/ffplay video

**Files:** Modify `include/opal/tunnel.hpp`, `src/tunnel.cpp`, `include/opal/tunnel_access.hpp`, `src/tunnel_access.cpp`, `src/zrok_cleanup.cpp`, `include/opal/session.hpp`, `src/session.cpp`, `include/opal/host.hpp`, `src/host.cpp`, `src/client.cpp`, `src/setup.cpp`, `src/media.cpp`, `include/opal/media.hpp`, `src/system.cpp`, `README.md`, `Makefile`, `tests/test_tunnel.cpp`, `tests/test_tunnel_recovery.cpp`, `tests/test_setup.cpp`, `tests/test_session.cpp`, `tests/test_media.cpp`, `tests/test_daemon.cpp`, `tests/test_hardening.cpp`, `tests/integration.sh`, `tests/smoke.sh`.

**Final interfaces:**

```cpp
struct TunnelAccessHandle { pid_t control_pid=-1; int control_port=0; };
bool tunnel_access_start(TunnelAccessHandle&,const std::string& control_token,int timeout_ms=30000);
```

New connection code: `opal:CONTROL`. Parser still accepts `opal:CONTROL,LEGACY_VIDEO` only so migration/cleanup can find the old video share; it is never used to carry media. `SessionOptions` removes `video_port` and `video_token`.

- [ ] **Step 1 — RED:** tunnel/setup tests require exactly one zrok `tcpTunnel` share and one-token code; legacy code parsing returns control + legacy token. Hardening/source tests require no production `ffplay`, no listener/access/share on 47991, no `VIDEO <token>` request path, no `video_backpressure_timeout_ms`. Rewrite integration to start Xvfb + synthetic capture, establish direct loopback UDP, recover from deliberate control-generation drop and present fresh video again without port 47991/ffplay.
- [ ] **Step 2 — Prove RED:** `make test-tunnel test-setup test-session test-hardening test-daemon`.
- [ ] **Step 3 — GREEN / atomic production switch:** remove `OPAL_DIRECT_VIDEO_DEV` gating and make authenticated control startup synchronously negotiate direct UDP before media starts. Client starts `VideoReceiver`; host starts `VideoSender`; a 5s failure returns `Direct UDP video could not be established. This network/NAT does not permit OPAL's direct-only video path.` No alternate media path is invoked.

  `tunnel_host_setup/start` create/run only control share. `tunnel_access_start` launches only control access. Host no longer listens on 47991. Remove TLS video workers/tokens/leases/request protocol, ffplay command/process, FLV-over-TLS copy loop and video backpressure constant.

  Legacy cleanup: if `host.ini` has `tunnel.video_token`, `retire_legacy_video_share(token)` lists the exact share, deletes it if present, re-lists up to five times at 100 ms intervals, and writes `video_token=""` only after confirmed absence. Failure prints warning but never activates it. `opal clean` continues recognizing legacy two-token saved addresses so old resources can still be deleted.

  Update README/doctor to state: zrok is control-only; direct UDP is mandatory for media; symmetric NAT/CGNAT pairs that cannot hole-punch will fail video by design.
- [ ] **Step 4 — Prove GREEN:** start Xvfb and run `DISPLAY=:99 make clean test`; then run the rewritten integration directly. Verify no process opens TCP 47991 in the fixture.
- [ ] **Step 5 — Commit:** `git commit -m "refactor: remove relayed OPAL video path"`.

---

### Task 12: Stress packet loss/reordering/rekey and verify exact shipping SHA

**Files:** Create `tests/test_direct_video_stress.cpp`; modify `Makefile`, `.github/workflows/ci.yml`; review all Task 1-11 changes.

- [ ] **Step 1 — Add deterministic stress coverage:** fixed seed, at least 50 control/media generations; each sends 120 frames, drops 0-5% ordinary packets, reorders adjacent packets, forces single-fragment FEC recoveries, forces at least one two-fragment reference loss/IDR recovery, rotates one control generation, then injects an old-generation authenticated packet and verifies rejection. Assert sender <=2 frames/16 MiB, receiver <=3 frames/16 MiB, fresh presentation after each recovery, and no stale ordinary retransmission.
- [ ] **Step 2 — Run stress:** `make test-direct-video-stress`. If it passes immediately, record it as valid new coverage; do not manufacture a failure. If it fails, preserve exact seed/output, apply systematic debugging to that demonstrated defect, and rerun.
- [ ] **Step 3 — Expand CI:** stress job runs `make all test-net test-session test-direct-video-session test-direct-video-pipeline test-direct-video-stress test-tunnel-recovery` and repeats session+direct stress three times. ASan/UBSan includes crypto/packet/reassembly/capture/decoder/audio/feedback/direct pipeline/stress. TSan includes session/direct-media shared-state tests.
- [ ] **Step 4 — Full local gate:** with Xvfb, `DISPLAY=:99 make clean test`.
- [ ] **Step 5 — Commit coverage:** `git commit -m "test: stress direct UDP video recovery"` if Task 12 changed files.
- [ ] **Step 6 — Exact-head GitHub gate:** record `git rev-parse HEAD`; require one GitHub Actions run with `head_sha` exactly equal to it and all four jobs `linux`, `sanitize`, `thread-sanitize`, `stress` concluded `success`.
- [ ] **Step 7 — Final spec audit:** explicitly verify 1080p60 default; temporary overrides; direct UDP only; 5s negotiation; exporter/ChaCha keys; 1024 replay; 1200-byte cap; 250ms IDR; sender/receiver bounds; XOR FEC; no stale retransmit; libavcodec; X11/GLX latest-frame presentation; <=40ms non-master audio; 100ms feedback; telemetry; rekey/re-negotiation; clear direct/STUN/probe/capture/decoder/path errors; no ffplay; no zrok video; no TCP video; legacy token cleanup only; exact-head four-lane CI green.
