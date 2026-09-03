# OPAL Direct-UDP Ultra-Low-Latency Video Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace OPAL's relayed ordered video stream with a latency-first 1080p60 direct encrypted UDP media path that drops stale work instead of buffering behind real time, while preserving reliable TLS/zrok control and input.

**Architecture:** Keep pairing, authentication, keyboard, mouse, control recovery, Wake-on-LAN, and zrok on the existing reliable control session. Build a separate direct UDP media stack with STUN candidate discovery, TLS-exporter-derived ChaCha20-Poly1305 keys, frame-aware fragmentation/FEC/reassembly, libavcodec decode, X11/GLX latest-frame presentation, bounded audio, bitrate pacing, and latency telemetry. Do not cut production video over until the direct path is tested end-to-end; after cutover, remove the old zrok video share/access, video TLS listener, ffplay player, and all media fallbacks.

**Tech Stack:** C++20, OpenSSL 3/TLS 1.3, POSIX UDP sockets, STUN/RFC 8489 binding discovery, GPU Screen Recorder, FFmpeg/libavformat/libavcodec/libavutil/libswresample, X11/XInput2, OpenGL/GLX, PulseAudio, Make, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-09-03-direct-udp-ultra-low-latency-video-design.md`

## Global Constraints

- Work directly on `main`, as requested for OPAL development.
- Plain `opal` defaults to a 1920x1080 ceiling at 60 FPS; hosts below that size are never upscaled.
- `--mode max|1080p|1440p|4k` and `--fps 15-240` remain temporary per-connection overrides and are never persisted.
- Direct encrypted UDP is the only production video transport after cutover.
- No TURN media relay, no zrok UDP media fallback, and no TCP video fallback.
- Direct UDP negotiation has a hard 5,000 ms deadline after authenticated candidate exchange.
- Maximum OPAL UDP datagram size is 1,200 bytes total, including OPAL header, ciphertext, and 16-byte AEAD tag.
- Video encryption is ChaCha20-Poly1305 with fresh direction-specific keys derived from the current authenticated TLS 1.3 control connection.
- Replay protection is a fixed 1,024-packet window per direction and control generation.
- Default IDR cadence is 250 ms; at 60 FPS this is approximately every 15 frames.
- Sender retains at most 2 encoded video access units and at most 16 MiB of queued encoded video, whichever bound is reached first.
- Receiver retains at most 3 in-progress video access units and at most 16 MiB of reassembly state, whichever bound is reached first.
- First FEC implementation uses one XOR parity fragment for each group of up to 10 ordinary media fragments and never waits across multiple frames.
- Ordinary stale P-frame fragments are never retransmitted.
- Presentation keeps at most one unpublished decoded frame and never runs a conventional playback/jitter queue.
- Audio is never the video master clock; native audio buffering is capped at 40 ms and stale audio is dropped.
- Receiver video feedback is sent at most once every 100 ms and never blocks current media delivery.
- Control/input remain on the current authenticated reliable TLS/zrok channel with the existing control-generation supervisor.
- Any production commit follows TDD: add the failing regression first, observe the expected RED result, implement the smallest production change, then verify GREEN.
- Each task ends with the relevant local test target and the pushed `main` CI run green before the next task starts, except the intentionally RED test commit used to prove the regression.
- Final completion requires Linux, ASan/UBSan, TSan, and reconnect/transport stress jobs green on the exact final `main` SHA.

---

## File Structure

The migration deliberately keeps `session.cpp` and `host.cpp` as orchestrators instead of growing them further.

- `include/opal/media_profile.hpp`, `src/media_profile.cpp`: stream defaults, mode parsing, bitrate/profile policy.
- `include/opal/video_capture.hpp`, `src/video_capture.cpp`: capture subprocess ownership plus libavformat demux into encoded access units.
- `include/opal/udp_transport.hpp`, `src/udp_transport.cpp`: UDP socket lifecycle, local candidates, STUN discovery, datagram I/O, peer addressing.
- `include/opal/video_crypto.hpp`, `src/video_crypto.cpp`: TLS exporter key derivation, ChaCha20-Poly1305, nonce construction, replay window.
- `include/opal/video_packet.hpp`, `src/video_packet.cpp`: fixed wire header, serialization/parsing, MTU-safe fragmentation, XOR parity generation.
- `include/opal/video_reassembly.hpp`, `src/video_reassembly.cpp`: bounded frame fragment state, parity recovery, freshness/drop policy.
- `include/opal/direct_video_session.hpp`, `src/direct_video_session.cpp`: authenticated candidate exchange, UDP probing, five-second path selection, generation binding.
- `include/opal/video_decoder.hpp`, `src/video_decoder.cpp`: low-delay H.264/AAC decode primitives.
- `include/opal/video_present.hpp`, `src/video_present.cpp`: OPAL-owned fullscreen X11/GLX latest-frame renderer.
- `include/opal/audio_output.hpp`, `src/audio_output.cpp`: bounded PulseAudio output and stale-audio dropping.
- `include/opal/video_feedback.hpp`, `src/video_feedback.cpp`: loss/RTT/freshness feedback, bitrate adaptation, clock-offset/latency telemetry.
- `include/opal/video_sender.hpp`, `src/video_sender.cpp`: host media queue, pacing, packetization/encryption/send, IDR restart handling.
- `include/opal/video_receiver.hpp`, `src/video_receiver.cpp`: client UDP receive/auth/reassembly/decode/present pipeline.
- `src/session.cpp`: client control-session orchestration only; starts/stops `DirectVideoSession`/`VideoReceiver` per control generation.
- `src/host.cpp`: host authentication/input orchestration only; starts/stops one direct media sender for the authenticated control session.
- `src/tunnel.cpp`, `src/tunnel_access.cpp`: final control-only zrok lifecycle and legacy two-token migration support.

---

### Task 1: Make 1080p60 the default and isolate media-profile policy

**Files:**
- Create: `include/opal/media_profile.hpp`
- Create: `src/media_profile.cpp`
- Create: `tests/test_media_profile.cpp`
- Modify: `include/opal/media.hpp`
- Modify: `src/media.cpp`
- Modify: `include/opal/client.hpp`
- Modify: `include/opal/setup.hpp`
- Modify: `include/opal/session.hpp`
- Modify: `src/main.cpp`
- Modify: `tests/test_setup.cpp`
- Modify: `tests/smoke.sh`
- Modify: `Makefile`

**Interfaces:**
- Produces:

```cpp
namespace opal {
struct StreamOptions {
    int max_width = 1920;
    int max_height = 1080;
    int fps = 60;
};

StreamOptions default_stream_options();
bool stream_mode_limit(const std::string &mode,int &max_width,int &max_height);
int automatic_bitrate_kbps(int width,int height,int fps);
}
```

- `media.hpp` continues to own subprocess helpers only; users of `StreamOptions` include `media_profile.hpp` directly.

- [ ] **Step 1: Write the failing profile/default tests**

Create `tests/test_media_profile.cpp` with assertions equivalent to:

```cpp
#include <opal/media_profile.hpp>
#include <cassert>

int main() {
    auto d=opal::default_stream_options();
    assert(d.max_width==1920);
    assert(d.max_height==1080);
    assert(d.fps==60);

    int w=-1,h=-1;
    assert(opal::stream_mode_limit("max",w,h)&&w==0&&h==0);
    assert(opal::stream_mode_limit("1080p",w,h)&&w==1920&&h==1080);
    assert(opal::stream_mode_limit("1440p",w,h)&&w==2560&&h==1440);
    assert(opal::stream_mode_limit("4k",w,h)&&w==3840&&h==2160);
    assert(!opal::stream_mode_limit("invalid",w,h));

    assert(opal::automatic_bitrate_kbps(1920,1080,60)==30000);
    return 0;
}
```

Extend `tests/test_setup.cpp` so a plain `interactive_run()` forwards `1920,1080,60`, while an explicit `{0,0,60}` `--mode max` style object still forwards native resolution. Keep the existing assertions that no stream-width/height/FPS keys are written to config.

Change `tests/smoke.sh` to require help text describing plain `opal` as 1080p60.

- [ ] **Step 2: Run the focused tests and verify RED**

Run:

```bash
make test-media-profile test-setup
BIN="$PWD/build/opal" INPUT_BIN="$PWD/build/opal-input" ./tests/smoke.sh
```

Expected: compile failure because `opal/media_profile.hpp` and `default_stream_options()` do not exist, followed by stale native-resolution help/default expectations.

- [ ] **Step 3: Implement the extracted profile policy**

Move `StreamOptions`, `stream_mode_limit`, and `automatic_bitrate_kbps` out of `media.hpp/.cpp` into `media_profile.hpp/.cpp`. Define:

```cpp
StreamOptions default_stream_options(){ return {}; }
```

Because `StreamOptions` now initializes to `1920,1080,60`, existing default arguments such as `interactive_run(const StreamOptions &stream={})` automatically become 1080p60. `--mode max` remains explicit and sets width/height to zero.

Update `src/main.cpp` help to:

```text
opal                                      Wake and connect at up to 1080p / 60 FPS
```

- [ ] **Step 4: Run tests and verify GREEN**

Run:

```bash
make clean test-media-profile test-media test-setup all
BIN="$PWD/build/opal" INPUT_BIN="$PWD/build/opal-input" ./tests/smoke.sh
```

Expected: all listed targets pass and plain stream construction is 1080p60.

- [ ] **Step 5: Commit**

```bash
git add include/opal/media_profile.hpp src/media_profile.cpp include/opal/media.hpp src/media.cpp include/opal/client.hpp include/opal/setup.hpp include/opal/session.hpp src/main.cpp tests/test_media_profile.cpp tests/test_setup.cpp tests/smoke.sh Makefile
git commit -m "feat: default OPAL streaming to 1080p60"
```

---

### Task 2: Add native media dependencies and expose encoded media units

**Files:**
- Create: `include/opal/video_capture.hpp`
- Create: `src/video_capture.cpp`
- Create: `tests/test_video_capture.cpp`
- Modify: `src/media.cpp`
- Modify: `Makefile`
- Modify: `.github/workflows/ci.yml`
- Modify: `src/system.cpp`
- Modify: `README.md`

**Interfaces:**
- Produces:

```cpp
namespace opal {
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
    bool start(const StreamOptions &stream,int bitrate_kbps,bool audio,const std::string &portal_token_file);
    bool next(EncodedMediaUnit &unit,int timeout_ms);
    const std::vector<MediaConfig>& configs() const;
    void stop();
    ~VideoCapture();
};
}
```

- `VideoCapture::next()` returns only complete encoded packets/access units from libavformat; it never exposes FLV bytes to later networking code.

- [ ] **Step 1: Write the failing capture/demux regression**

Generate a bounded real fixture in `tests/test_video_capture.cpp` by starting `VideoCapture` with `OPAL_CAPTURE_CMD` set to:

```bash
ffmpeg -hide_banner -loglevel error \
  -re -f lavfi -i testsrc=size=320x180:rate=60 \
  -f lavfi -i sine=frequency=440:sample_rate=48000 \
  -c:v libx264 -preset ultrafast -tune zerolatency -bf 0 -g 15 -keyint_min 15 -sc_threshold 0 \
  -c:a aac -b:a 96k -f flv pipe:1
```

Assert within a five-second deadline:

```cpp
assert(capture.start({320,180,60},8000,true,""));
bool video=false,audio=false,key=false;
for(int i=0;i<300 && !(video&&audio&&key);++i){
    opal::EncodedMediaUnit u;
    if(!capture.next(u,1000)) continue;
    video |= u.kind==opal::MediaKind::VideoH264 && !u.data.empty();
    audio |= u.kind==opal::MediaKind::AudioAac && !u.data.empty();
    key |= u.kind==opal::MediaKind::VideoH264 && u.keyframe;
}
assert(video&&audio&&key);
assert(!capture.configs().empty());
```

Add command-generation assertions requiring GSR `-keyint 0.25`, H.264 CBR, `-tune performance`, and no one-second `-keyint 1`. The FFmpeg fallback must use `-g 15 -keyint_min 15` for 60 FPS.

- [ ] **Step 2: Run and verify RED**

Run:

```bash
make test-video-capture
```

Expected: compile failure because `video_capture.hpp` and `VideoCapture` do not exist.

- [ ] **Step 3: Add build dependencies and implement demux**

Update product/media linker flags using `pkg-config`:

```make
MEDIA_PKGS := libavformat libavcodec libavutil libswresample
MEDIA_CFLAGS := $(shell pkg-config --cflags $(MEDIA_PKGS))
MEDIA_LIBS := $(shell pkg-config --libs $(MEDIA_PKGS))
CPPFLAGS += $(MEDIA_CFLAGS)
LDLIBS += $(MEDIA_LIBS) -lGL
```

Update Ubuntu CI dependencies to include:

```text
pkg-config libavcodec-dev libavformat-dev libavutil-dev libswresample-dev libgl1-mesa-dev libpulse-dev xvfb
```

Use a custom `AVIOContext` whose read callback polls/reads the capture process fd. Open `flv` with `avformat_open_input`, call `avformat_find_stream_info`, copy H.264/AAC extradata into `MediaConfig`, and emit `AVPacket` payloads through `EncodedMediaUnit` with timestamps converted to microseconds.

Use a 250 ms keyframe interval in generated capture commands:

```cpp
const int gop=std::max(1,fps/4);
```

For GSR use `-keyint 0.25 -tune performance`; for FFmpeg use `-bf 0 -g gop -keyint_min gop -sc_threshold 0`.

Update `opal doctor` to stop treating `ffplay` as the future media requirement and report FFmpeg plus GPU Screen Recorder capture availability.

- [ ] **Step 4: Run and verify GREEN**

Run:

```bash
make clean test-video-capture test-media test-core all
```

Expected: fixture yields H.264 video, AAC audio, configuration data, and a keyframe; full product links against libav* and GL.

- [ ] **Step 5: Commit**

```bash
git add include/opal/video_capture.hpp src/video_capture.cpp tests/test_video_capture.cpp src/media.cpp Makefile .github/workflows/ci.yml src/system.cpp README.md
git commit -m "feat: expose encoded media units"
```

---

### Task 3: Add direct UDP sockets and STUN candidate discovery

**Files:**
- Create: `include/opal/udp_transport.hpp`
- Create: `src/udp_transport.cpp`
- Create: `tests/test_udp_transport.cpp`
- Modify: `Makefile`

**Interfaces:**
- Produces:

```cpp
namespace opal {
enum class CandidateType { Local, ServerReflexive };
struct StunEndpoint { std::string host; std::uint16_t port; };
struct UdpCandidate { std::string host; std::uint16_t port; CandidateType type; };
struct UdpSocket { int fd=-1; std::uint16_t local_port=0; };

UdpSocket open_udp_socket();
void close_udp_socket(UdpSocket &socket);
std::vector<UdpCandidate> local_udp_candidates(const UdpSocket &socket);
std::vector<StunEndpoint> default_stun_endpoints();
std::optional<UdpCandidate> discover_server_reflexive_candidate(
    const UdpSocket &socket,const std::vector<StunEndpoint> &servers,int timeout_ms);
bool send_datagram(int fd,const sockaddr_storage &peer,socklen_t peer_len,
                   std::span<const std::uint8_t> bytes);
int recv_datagram(int fd,std::span<std::uint8_t> buffer,
                  sockaddr_storage &peer,socklen_t &peer_len,int timeout_ms);
}
```

- Built-in STUN endpoints are `stun.cloudflare.com:3478` and `stunserver2025.stunprotocol.org:3478`; tests never depend on Internet access.

- [ ] **Step 1: Write the failing socket/STUN tests**

In `tests/test_udp_transport.cpp`, create two loopback UDP sockets and assert datagrams pass both directions. Start a fake local STUN server that validates the RFC 8489 binding request cookie `0x2112A442` and transaction id, then returns XOR-MAPPED-ADDRESS for `127.0.0.1:<client-port>`. Assert discovery returns that candidate.

Also send replies with the wrong transaction id and malformed attribute lengths; assert they are ignored and discovery times out within the supplied bound.

- [ ] **Step 2: Run and verify RED**

```bash
make test-udp-transport
```

Expected: missing `opal/udp_transport.hpp`.

- [ ] **Step 3: Implement nonblocking UDP/STUN primitives**

Create sockets with `SOCK_DGRAM|SOCK_CLOEXEC` where supported, bind to `0.0.0.0:0`, set nonblocking mode, and obtain the assigned port with `getsockname`.

Build a 20-byte STUN Binding Request:

```text
0x0001 message type
0x0000 body length
0x2112A442 magic cookie
12 random transaction-id bytes
```

Parse only bounded STUN responses with the matching transaction id. Support XOR-MAPPED-ADDRESS for IPv4 and IPv6. Unknown attributes are skipped using four-byte alignment. Never trust STUN data for authentication; it supplies routing candidates only.

- [ ] **Step 4: Run and verify GREEN**

```bash
make clean test-udp-transport test-net
```

Expected: loopback UDP, valid fake STUN, malformed-response rejection, and existing TLS network tests all pass.

- [ ] **Step 5: Commit**

```bash
git add include/opal/udp_transport.hpp src/udp_transport.cpp tests/test_udp_transport.cpp Makefile
git commit -m "feat: add direct UDP candidate discovery"
```

---

### Task 4: Derive TLS-bound video keys and protect datagrams

**Files:**
- Create: `include/opal/video_crypto.hpp`
- Create: `src/video_crypto.cpp`
- Create: `tests/test_video_crypto.cpp`
- Modify: `Makefile`

**Interfaces:**
- Produces:

```cpp
namespace opal {
struct VideoKeys {
    std::array<std::uint8_t,32> send_key{};
    std::array<std::uint8_t,32> recv_key{};
    std::array<std::uint8_t,12> send_nonce_base{};
    std::array<std::uint8_t,12> recv_nonce_base{};
};

bool derive_video_keys(SSL *ssl,std::string_view session_token,
                       std::string_view client_public_key,
                       std::string_view host_fingerprint,
                       bool client_side,VideoKeys &out);

bool seal_video_datagram(const VideoKeys &keys,std::uint64_t sequence,
                         std::span<const std::uint8_t> aad,
                         std::span<const std::uint8_t> plaintext,
                         std::vector<std::uint8_t> &ciphertext_and_tag);

bool open_video_datagram(const VideoKeys &keys,std::uint64_t sequence,
                         std::span<const std::uint8_t> aad,
                         std::span<const std::uint8_t> ciphertext_and_tag,
                         std::vector<std::uint8_t> &plaintext);

class ReplayWindow1024 {
public:
    bool accept(std::uint64_t sequence);
    void reset();
};
}
```

- [ ] **Step 1: Write failing crypto tests**

Use the same localhost TLS certificate/test-server pattern as `tests/test_net.cpp`. After TLS handshake, derive keys on both endpoints with the same token/client key/fingerprint. Assert:

```cpp
assert(client.send_key==server.recv_key);
assert(client.recv_key==server.send_key);
```

Seal a 1,000-byte payload with AAD and sequence 7, open it on the peer, and compare bytes. Flip one ciphertext byte and assert open fails. Change AAD and assert open fails.

For replay protection assert:

```cpp
ReplayWindow1024 w;
assert(w.accept(1000));
assert(!w.accept(1000));
assert(w.accept(1500));
assert(!w.accept(400)); // more than 1024 behind highest
```

- [ ] **Step 2: Run and verify RED**

```bash
make test-video-crypto
```

Expected: missing `video_crypto.hpp`.

- [ ] **Step 3: Implement exporter/AEAD/replay logic**

Call `SSL_export_keying_material` with label:

```text
EXPORTER-OPAL-DIRECT-VIDEO-v1
```

and context bytes:

```text
session_token + "\n" + client_public_key + "\n" + host_fingerprint
```

Request 88 bytes and map them as host->client key 32, client->host key 32, host nonce base 12, client nonce base 12. Swap send/recv views on the client side.

Build each 12-byte nonce by copying the direction nonce base and XORing the big-endian 64-bit packet sequence into the final eight bytes. Use `EVP_chacha20_poly1305()` with a 16-byte tag.

Implement `ReplayWindow1024` as a 1,024-bit sliding bitmap keyed by highest accepted sequence; do not allocate based on attacker-provided sequence values.

- [ ] **Step 4: Run and verify GREEN**

```bash
make clean test-video-crypto test-net test-hardening
```

Expected: exporter directions match, AEAD roundtrip succeeds, tamper/AAD mismatch fail, duplicate/too-old packets are rejected.

- [ ] **Step 5: Commit**

```bash
git add include/opal/video_crypto.hpp src/video_crypto.cpp tests/test_video_crypto.cpp Makefile
git commit -m "feat: authenticate direct video datagrams"
```

---

### Task 5: Define frame-aware packets, fragmentation, XOR FEC, and bounded reassembly

**Files:**
- Create: `include/opal/video_packet.hpp`
- Create: `src/video_packet.cpp`
- Create: `include/opal/video_reassembly.hpp`
- Create: `src/video_reassembly.cpp`
- Create: `tests/test_video_packet.cpp`
- Create: `tests/test_video_reassembly.cpp`
- Modify: `Makefile`

**Interfaces:**
- Wire header is exactly 52 bytes before ciphertext:

```cpp
enum class VideoMediaType : std::uint8_t {
    VideoH264=1, AudioAac=2, Probe=3, ProbeAck=4, Fec=5
};

enum VideoFrameFlags : std::uint16_t {
    FrameKeyframe=1u<<0,
    FrameConfig=1u<<1,
    FrameEnd=1u<<2
};

struct VideoPacketHeader {
    std::uint32_t magic=0x4f505631; // "OPV1"
    std::uint8_t version=1;
    VideoMediaType media_type=VideoMediaType::VideoH264;
    std::uint16_t flags=0;
    std::uint32_t generation=0;
    std::uint64_t session_id=0;
    std::uint64_t packet_sequence=0;
    std::uint64_t frame_id=0;
    std::uint64_t capture_timestamp_us=0;
    std::uint16_t fragment_index=0;
    std::uint16_t fragment_count=0;
    std::uint16_t fec_group=0;
    std::uint16_t payload_length=0;
};
```

- Maximum encrypted plaintext payload is `1200 - 52 - 16 = 1132` bytes.
- Produces `serialize_video_header`, `parse_video_header`, `fragment_media_unit`, `make_xor_fec`, and `VideoReassembler::push(...)`.

- [ ] **Step 1: Write failing packet/reassembly tests**

Cover:

```text
network-byte-order roundtrip for every header field
reject wrong magic/version
reject payload_length inconsistent with datagram length
fragment a 100 KiB H.264 access unit into <=1200-byte datagrams
all fragment_count/index fields bounded and consistent
one XOR parity packet per <=10 data fragments
recover exactly one missing fragment in a FEC group
fail recovery when two fragments in one FEC group are absent
complete frame bytes equal original bytes
fourth in-progress frame evicts the oldest stale ordinary frame
reassembly memory never exceeds 16 MiB
new generation resets all old frame state
```

- [ ] **Step 2: Run and verify RED**

```bash
make test-video-packet test-video-reassembly
```

Expected: missing packet/reassembly headers.

- [ ] **Step 3: Implement fixed serialization and bounded state**

Write all integer fields explicitly with `htons`/`htonl` plus a small 64-bit network-order helper; never cast an unaligned network buffer to `VideoPacketHeader*`.

`fragment_media_unit()` reserves 1,132 plaintext bytes per ordinary fragment. FEC parity plaintext starts with:

```text
uint8 data_fragment_count
uint16 original_length[10]
XOR payload bytes up to longest fragment
```

The reassembler stores at most three `FrameAssembly` objects and tracks total allocated bytes before accepting fragment data. On a fourth newer frame, drop the oldest incomplete ordinary frame. If the dropped frame may break H.264 reference continuity, return a status requesting an IDR instead of preserving stale data.

- [ ] **Step 4: Run and verify GREEN**

```bash
make clean test-video-packet test-video-reassembly
```

Expected: all serialization, size-bound, parity recovery, freshness, and memory-bound cases pass under normal and sanitizer builds.

- [ ] **Step 5: Commit**

```bash
git add include/opal/video_packet.hpp src/video_packet.cpp include/opal/video_reassembly.hpp src/video_reassembly.cpp tests/test_video_packet.cpp tests/test_video_reassembly.cpp Makefile
git commit -m "feat: add frame-aware video datagrams"
```

---

### Task 6: Negotiate one authenticated direct UDP path over the control session

**Files:**
- Create: `include/opal/direct_video_session.hpp`
- Create: `src/direct_video_session.cpp`
- Create: `tests/test_direct_video_session.cpp`
- Modify: `include/opal/session.hpp`
- Modify: `src/session.cpp`
- Modify: `src/host.cpp`
- Modify: `Makefile`

**Interfaces:**
- Produces:

```cpp
struct DirectVideoPath {
    UdpSocket socket;
    sockaddr_storage peer{};
    socklen_t peer_len=0;
    VideoKeys keys;
    std::uint64_t session_id=0;
    std::uint32_t generation=0;
};

using ControlSend = std::function<bool(const std::string&,int)>;
using ControlRead = std::function<bool(std::string&,int)>;

bool negotiate_client_direct_video(SSL *control_ssl,
    const std::string &session_token,const std::string &client_public_key,
    const std::string &host_fingerprint,std::uint32_t generation,
    const std::vector<StunEndpoint> &stun_servers,
    ControlSend send,ControlRead read,DirectVideoPath &out,std::string &error,
    int deadline_ms=5000);

bool negotiate_host_direct_video(SSL *control_ssl,
    const std::string &session_token,const std::string &client_public_key,
    const std::string &host_fingerprint,std::uint32_t generation,
    const std::vector<StunEndpoint> &stun_servers,
    ControlSend send,ControlRead read,DirectVideoPath &out,std::string &error,
    int deadline_ms=5000);
```

- Control lines are strictly bounded and use one candidate per line:

```text
UDP_CANDIDATE <generation> <L|S> <host> <port>
UDP_CANDIDATES_DONE <generation>
UDP_PROBE_READY <generation>
UDP_SELECTED <generation> <host> <port>
REQUEST_IDR <generation>
```

- UDP probe/probe-ack datagrams use the same video keys and packet format, so candidate possession alone cannot authenticate a peer.

- [ ] **Step 1: Write failing negotiation tests**

Build a localhost TLS control fixture with client and host negotiators running concurrently. Supply only local candidates and assert they select each other within 1 second and derive opposite send/receive keys.

Inject a UDP probe with an invalid AEAD tag and assert it does not select a path. Supply unreachable candidates with an accelerated `deadline_ms=250` and assert failure is bounded and returns exactly:

```text
Direct UDP video could not be established. This network/NAT does not permit OPAL's direct-only video path.
```

Add parser tests rejecting more than 16 candidates, invalid generation numbers, oversized host strings, invalid ports, and extra tokens.

- [ ] **Step 2: Run and verify RED**

```bash
make test-direct-video-session
```

Expected: missing direct-video session interface.

- [ ] **Step 3: Implement candidate exchange and authenticated hole punching**

Open one UDP socket before exchange, gather local plus at most one server-reflexive candidate, send no more than 16 candidate lines, and wait for the peer's bounded list. After both `*_DONE` lines, both peers send authenticated `Probe` datagrams to all candidate combinations at 50 ms intervals until a valid `ProbeAck` identifies the selected source address.

Use one absolute deadline; STUN, candidate exchange, and probe loops may not reset it. LAN candidates are attempted first, but the first authenticated working path wins.

At this task the old production media stream remains active; direct negotiation is exercised only by the focused test/development path. Do not add any UDP media fallback behavior.

- [ ] **Step 4: Run and verify GREEN**

```bash
make clean test-direct-video-session test-session test-net test-udp-transport test-video-crypto
```

Expected: authenticated loopback negotiation succeeds, forged probes fail, direct-only failure is bounded and explicit, existing control recovery still passes.

- [ ] **Step 5: Commit**

```bash
git add include/opal/direct_video_session.hpp src/direct_video_session.cpp tests/test_direct_video_session.cpp include/opal/session.hpp src/session.cpp src/host.cpp Makefile
git commit -m "feat: negotiate authenticated direct UDP video"
```

---

### Task 7: Add low-delay H.264 decode and OPAL-owned X11/GLX presentation

**Files:**
- Create: `include/opal/video_decoder.hpp`
- Create: `src/video_decoder.cpp`
- Create: `include/opal/video_present.hpp`
- Create: `src/video_present.cpp`
- Create: `tests/test_video_decoder.cpp`
- Create: `tests/test_video_present.cpp`
- Modify: `Makefile`
- Modify: `.github/workflows/ci.yml`

**Interfaces:**
- Produces:

```cpp
struct DecodedVideoFrame {
    AVFrame *frame=nullptr;
    std::int64_t pts_us=0;
};

class VideoDecoder {
public:
    bool configure_h264(std::span<const std::uint8_t> extradata);
    bool decode(std::span<const std::uint8_t> access_unit,std::int64_t pts_us,
                std::vector<DecodedVideoFrame> &frames);
    void flush();
    ~VideoDecoder();
};

class VideoPresenter {
public:
    bool open(int source_width,int source_height,bool fullscreen=true);
    bool present(DecodedVideoFrame frame);
    Window x11_window() const;
    std::pair<int,int> drawable_size() const;
    void close();
    ~VideoPresenter();
};
```

- `DecodedVideoFrame` ownership is explicit: `VideoPresenter::present` consumes the `AVFrame*`; any replaced unpublished frame is freed immediately.

- [ ] **Step 1: Write failing decoder/presenter tests**

Generate a tiny Annex-B H.264 sample in the decoder test with:

```bash
ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc=size=320x180:rate=60 \
  -frames:v 4 -c:v libx264 -preset ultrafast -tune zerolatency -bf 0 -g 15 \
  -f h264 pipe:1
```

Feed access units to `VideoDecoder`, assert at least one 320x180 frame appears without waiting for future B-frames, and assert `flush()` frees/restarts decoder state.

For presenter tests, launch `Xvfb :99 -screen 0 1280x720x24`, set `DISPLAY=:99`, open a non-fullscreen 320x180 presenter, submit three frames faster than drawing, and assert the presenter reports no more than one unpublished frame through a test-visible `pending_frame_count()==0|1` accessor compiled into the normal class.

- [ ] **Step 2: Run and verify RED**

```bash
make test-video-decoder test-video-present
```

Expected: missing decoder/presenter interfaces.

- [ ] **Step 3: Implement low-delay decode and GLX renderer**

Configure the H.264 decoder with:

```cpp
ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
ctx->flags2 |= AV_CODEC_FLAG2_FAST;
ctx->thread_count = 1;
ctx->thread_type = 0;
```

Use `avcodec_send_packet`/`avcodec_receive_frame`. Reject unexpected B-frame/reordered behavior from the configured encoder path rather than adding a playback queue.

Create the presentation X11 window on the current `DISPLAY`, create one GLX context, and upload planar YUV420P into Y/U/V textures. Convert YUV to RGB in a fragment shader. For NV12, upload Y plus interleaved UV and use the matching shader branch. Only if the decoder returns another pixel format may a bounded conversion path be used.

Swap immediately after drawing and do not introduce an application-level presentation clock.

- [ ] **Step 4: Run and verify GREEN**

```bash
Xvfb :99 -screen 0 1280x720x24 >/tmp/opal-xvfb.log 2>&1 & XVFB=$!
DISPLAY=:99 make clean test-video-decoder test-video-present
kill "$XVFB"
```

Expected: deterministic software decode and GLX presentation pass under Xvfb/Mesa with at most one pending frame.

- [ ] **Step 5: Commit**

```bash
git add include/opal/video_decoder.hpp src/video_decoder.cpp include/opal/video_present.hpp src/video_present.cpp tests/test_video_decoder.cpp tests/test_video_present.cpp Makefile .github/workflows/ci.yml
git commit -m "feat: add native low-delay video playback"
```

---

### Task 8: Stream real H.264 over direct UDP with freshness-first sender/receiver

**Files:**
- Create: `include/opal/video_sender.hpp`
- Create: `src/video_sender.cpp`
- Create: `include/opal/video_receiver.hpp`
- Create: `src/video_receiver.cpp`
- Create: `tests/test_direct_video_pipeline.cpp`
- Modify: `src/host.cpp`
- Modify: `src/session.cpp`
- Modify: `src/client.cpp`
- Modify: `Makefile`

**Interfaces:**
- Produces:

```cpp
class VideoSender {
public:
    bool start(DirectVideoPath path,const StreamOptions &stream,bool audio,
               std::function<void(const std::string&)> control_send);
    void request_idr();
    void stop();
};

class VideoReceiver {
public:
    bool start(DirectVideoPath path,
               std::function<void(const std::string&)> control_send);
    bool media_started() const;
    Window presentation_window() const;
    void stop();
};
```

- Sender queue bound: two video access units / 16 MiB.
- Receiver reassembly bound: three access units / 16 MiB.
- `REQUEST_IDR` is rate-limited to at most one effective capture restart every 250 ms.

- [ ] **Step 1: Write failing end-to-end direct-video test**

Use loopback direct paths and the real ffmpeg synthetic capture fixture. Start sender and receiver without any TCP video socket. Assert within five seconds:

```text
receiver media_started becomes true
at least one decoded frame is presented
no ffplay process is spawned
all UDP datagrams are <=1200 bytes
```

Add a packet-loss shim in the test sender that drops one data fragment in a FEC group and assert parity restores the frame without an IDR request. Drop two fragments from a key reference frame and assert the receiver calls `REQUEST_IDR`; after the sender restarts capture, presentation resumes from a fresh keyframe.

Add a stale-queue test that blocks UDP sends long enough to fill sender state, inserts a newer ordinary frame, and asserts the older ordinary frame is discarded instead of increasing queue depth beyond two.

- [ ] **Step 2: Run and verify RED**

```bash
DISPLAY=:99 make test-direct-video-pipeline
```

Expected: missing sender/receiver interfaces.

- [ ] **Step 3: Implement the production direct video pipeline**

`VideoSender` owns `VideoCapture`, converts `MediaConfig` into `FrameConfig` packets, assigns monotonic frame ids, fragments/FEC-protects media, encrypts each packet, and writes nonblocking UDP. On `EAGAIN` or queue overflow, drop the oldest queued ordinary P-frame; never block the capture thread waiting for stale network capacity.

`VideoReceiver` authenticates/decrypts, applies replay protection, reassembles/FEC-recovers frames, decodes H.264, and hands only the newest useful frame to `VideoPresenter`.

When continuity is lost, the receiver sends:

```text
REQUEST_IDR <generation>
```

The host handles this by setting `VideoSender::request_idr()`. Because the capture subprocess does not have a stable portable force-IDR control API, the first implementation satisfies the request by restarting `VideoCapture` at the current profile/bitrate, which yields fresh codec config plus a new keyframe. Rate-limit that restart to once per 250 ms.

Wire `SessionSupervisor` so each authenticated control generation negotiates direct UDP and starts one `VideoReceiver`; a generation change stops it, drops keys/state, renegotiates, and starts a fresh receiver. Wire the host's authenticated control client to own the matching `VideoSender`.

Do not silently start the old TLS video stream when direct negotiation fails.

- [ ] **Step 4: Run and verify GREEN**

```bash
DISPLAY=:99 make clean test-direct-video-pipeline test-direct-video-session test-video-packet test-video-reassembly test-video-decoder test-video-present test-session
```

Expected: real H.264 crosses only UDP in the direct test; loss/FEC/IDR recovery works; queues stay bounded; control session tests remain green.

- [ ] **Step 5: Commit**

```bash
git add include/opal/video_sender.hpp src/video_sender.cpp include/opal/video_receiver.hpp src/video_receiver.cpp tests/test_direct_video_pipeline.cpp src/host.cpp src/session.cpp src/client.cpp Makefile
git commit -m "feat: stream OPAL video over direct UDP"
```

---

### Task 9: Add latency-bounded AAC audio without making audio the master clock

**Files:**
- Create: `include/opal/audio_output.hpp`
- Create: `src/audio_output.cpp`
- Create: `tests/test_audio_output.cpp`
- Modify: `src/video_receiver.cpp`
- Modify: `Makefile`
- Modify: `.github/workflows/ci.yml`

**Interfaces:**
- Produces:

```cpp
class AudioOutput {
public:
    bool configure_aac(std::span<const std::uint8_t> extradata,
                       int sample_rate,int channels);
    bool submit(std::span<const std::uint8_t> aac_packet,
                std::int64_t pts_us,std::int64_t current_video_pts_us);
    void reset_to(std::int64_t video_pts_us);
    std::uint32_t queued_ms() const;
    void close();
};
```

- PulseAudio stream attributes target a 40 ms maximum queue, 0 prebuffer, and roughly 10 ms minimum request granularity.

- [ ] **Step 1: Write failing audio queue tests**

Use an injectable sink callback in the test constructor/implementation internals so unit tests do not require a running PulseAudio daemon. Feed timestamped AAC fixture packets and assert:

```text
queued_ms never reports >40
packets older than current_video_pts_us-40000 are dropped
reset_to discards queued stale samples immediately
video timestamps are accepted regardless of audio output readiness
```

Decode a short AAC fixture and assert non-empty PCM is produced at the configured sample rate/channels.

- [ ] **Step 2: Run and verify RED**

```bash
make test-audio-output
```

Expected: missing audio output interface.

- [ ] **Step 3: Implement AAC decode/resample and PulseAudio output**

Use libavcodec AAC decode and libswresample to signed 16-bit interleaved PCM. Use the asynchronous libpulse API with a playback `pa_buffer_attr` whose `tlength` and `maxlength` equal 40 ms worth of PCM, `minreq` is about 10 ms, and `prebuf=0`.

Before decoding/submitting each packet, compare its PTS to the newest video timeline. Drop audio more than 40 ms behind; never wait to present video for audio synchronization.

Wire `VideoReceiver` to route `AudioAac` assemblies to `AudioOutput` independently of the video decode/present path.

- [ ] **Step 4: Run and verify GREEN**

```bash
make clean test-audio-output test-direct-video-pipeline
```

Expected: AAC decoding works, test sink never exceeds 40 ms, and direct video tests remain independent of audio readiness.

- [ ] **Step 5: Commit**

```bash
git add include/opal/audio_output.hpp src/audio_output.cpp tests/test_audio_output.cpp src/video_receiver.cpp Makefile .github/workflows/ci.yml
git commit -m "feat: add latency-bounded direct audio"
```

---

### Task 10: Add pacing, authenticated feedback, adaptive bitrate, clock sync, and latency telemetry

**Files:**
- Create: `include/opal/video_feedback.hpp`
- Create: `src/video_feedback.cpp`
- Create: `tests/test_video_feedback.cpp`
- Modify: `src/video_sender.cpp`
- Modify: `src/video_receiver.cpp`
- Modify: `src/session.cpp`
- Modify: `src/host.cpp`
- Modify: `Makefile`

**Interfaces:**
- Produces:

```cpp
struct VideoFeedbackSample {
    std::uint64_t highest_sequence=0;
    std::uint32_t received=0;
    std::uint32_t lost=0;
    std::uint32_t rtt_us=0;
    std::uint32_t decode_age_us=0;
};

class BitrateController {
public:
    explicit BitrateController(int ceiling_kbps);
    int on_feedback(const VideoFeedbackSample &sample,
                    std::chrono::steady_clock::time_point now);
    int target_kbps() const;
};

struct LatencyTelemetry {
    double capture_to_packet_ms=0;
    double network_ms=0;
    double reassembly_ms=0;
    double decode_ms=0;
    double present_ms=0;
    double total_ms=0;
    double loss_percent=0;
    std::uint64_t stale_frames=0;
    int bitrate_kbps=0;
};
```

- Control messages:

```text
VIDEO_FEEDBACK <generation> <highest_seq> <received> <lost> <rtt_us> <decode_age_us>
CLOCK_SYNC <generation> <t0_us> <t1_us> <t2_us>
```

- [ ] **Step 1: Write failing controller/telemetry tests**

Assert exact initial adaptation policy:

```text
initial target = automatic_bitrate_kbps(requested geometry/fps)
if loss >2% OR RTT inflation >15 ms for 2 consecutive feedback samples: target *=0.80
floor = max(4000 kbps, ceiling*35/100)
if loss <0.2% and RTT inflation <3 ms continuously for >=1 second: target *=1.05
never exceed initial ceiling
capture restart/reconfigure only when target differs by >=15% and last restart was >=2 seconds ago
```

Assert feedback serialization rejects extra fields and that receiver generation mismatch is ignored.

Test the clock-offset estimator with known synthetic timestamps and assert one-way latency calculation stays within 1 ms of the fixture value.

- [ ] **Step 2: Run and verify RED**

```bash
make test-video-feedback
```

Expected: missing feedback/controller interfaces.

- [ ] **Step 3: Implement freshness-oriented pacing and adaptation**

Add a token-bucket sender pacer at the current target bitrate with a maximum burst of two 1,200-byte datagrams. If pacing would require preserving an ordinary frame beyond sender queue bounds, discard that frame instead of sleeping behind the live edge.

`VideoReceiver` sends feedback no more often than once every 100 ms over the already authenticated control channel. `VideoSender` lowers bitrate immediately under two consecutive overload samples and raises it only after one second healthy.

When a bitrate change is at least 15% and two seconds have elapsed since the prior capture restart, restart `VideoCapture` at the new CBR target so encoder output follows path capacity and begins with fresh codec config/IDR.

Maintain EWMA telemetry with alpha `0.2`. Under `OPAL_DEBUG=1`, print one report per second containing capture->packet, network, reassembly, decode, present, total, loss, stale-frame count, and bitrate. Telemetry never blocks frame delivery.

- [ ] **Step 4: Run and verify GREEN**

```bash
make clean test-video-feedback test-direct-video-pipeline test-session
```

Expected: deterministic bitrate decrease/increase behavior, bounded pacing, valid clock estimation, and direct pipeline remains live under synthetic congestion.

- [ ] **Step 5: Commit**

```bash
git add include/opal/video_feedback.hpp src/video_feedback.cpp tests/test_video_feedback.cpp src/video_sender.cpp src/video_receiver.cpp src/session.cpp src/host.cpp Makefile
git commit -m "perf: adapt direct video for minimum latency"
```

---

### Task 11: Remove the relayed video architecture and make connection codes control-only

**Files:**
- Modify: `include/opal/tunnel.hpp`
- Modify: `src/tunnel.cpp`
- Modify: `include/opal/tunnel_access.hpp`
- Modify: `src/tunnel_access.cpp`
- Modify: `src/zrok_cleanup.cpp`
- Modify: `include/opal/session.hpp`
- Modify: `src/session.cpp`
- Modify: `include/opal/host.hpp`
- Modify: `src/host.cpp`
- Modify: `src/client.cpp`
- Modify: `src/setup.cpp`
- Modify: `src/media.cpp`
- Modify: `include/opal/media.hpp`
- Modify: `tests/test_tunnel.cpp`
- Modify: `tests/test_tunnel_recovery.cpp`
- Modify: `tests/test_setup.cpp`
- Modify: `tests/test_session.cpp`
- Modify: `tests/test_media.cpp`
- Modify: `tests/test_daemon.cpp`
- Modify: `tests/test_hardening.cpp`
- Modify: `tests/integration.sh`
- Modify: `tests/smoke.sh`
- Modify: `src/system.cpp`
- Modify: `README.md`
- Modify: `Makefile`

**Interfaces:**
- Final tunnel access handle:

```cpp
struct TunnelAccessHandle {
    pid_t control_pid=-1;
    int control_port=0;
};

bool tunnel_access_start(TunnelAccessHandle &handle,
                         const std::string &control_token,
                         int timeout_ms=30000);
```

- New connection code is `opal:CONTROL`.
- Parser accepts both `opal:CONTROL` and legacy `opal:CONTROL,VIDEO`; legacy video token is exposed only to migration/cleanup code and is never used for media.
- Final `SessionOptions` removes `video_port` and `video_token`.

- [ ] **Step 1: Write failing removal/migration regressions**

Update tunnel/setup tests to assert new host setup creates only one persistent `tcpTunnel` share and prints a one-token code:

```text
opal:opal-ctl-...
```

Assert parsing:

```cpp
assert(tunnel_connection_code("opal:ctl",&control,&legacy_video));
assert(control=="ctl"&&legacy_video.empty());
assert(tunnel_connection_code("opal:ctl,oldvid",&control,&legacy_video));
assert(control=="ctl"&&legacy_video=="oldvid");
```

Update hardening/source tests to require:

```text
no ffplay string in production source
no host listener on video port 47991
no zrok video share/access launch
no VIDEO token/request protocol
no video_backpressure_timeout_ms constant
```

Integration must start Xvfb, use the synthetic FFmpeg capture fixture, connect control through localhost, establish direct loopback UDP, recover after a deliberate control-generation drop, and present again without spawning ffplay or opening port 47991.

- [ ] **Step 2: Run and verify RED**

```bash
make test-tunnel test-setup test-session test-hardening test-daemon
```

Expected: current implementation still creates/uses video share, video access, video TLS listener, and ffplay.

- [ ] **Step 3: Delete the old runtime path and migrate legacy tokens**

`tunnel_host_setup()` creates only a control share. `tunnel_host_start()` launches only that control share. `tunnel_access_start()` launches only one local control access process.

Keep legacy parser/cleanup support. On host startup, if `host.ini` still contains `tunnel.video_token`, call a bounded `retire_legacy_video_share(token)` helper that:

```text
lists the exact saved share token
if present, deletes that exact share
re-lists up to 5 times with 100 ms spacing
only after confirmed absence writes video_token="" to host.ini
on failure prints a warning but never uses the legacy share for media
```

Remove the 47991 TLS listener/workers, session video tokens/leases, `VIDEO ...` request protocol, ffplay process/player command, player stdin timeout, FLV-over-TLS forwarding, and host video backpressure constant. Keep capture subprocess helpers still needed by `VideoCapture`; remove `SinkProcess` helpers if input injection no longer depends on the generic sink implementation, or retain only a renamed generic process sink used by input with no video-specific APIs.

Update README/doctor/dependencies to describe zrok as control-only and direct UDP as mandatory for video. Explicitly document that some symmetric NAT/CGNAT combinations cannot establish video under the direct-only policy.

- [ ] **Step 4: Run the complete suite and verify GREEN**

```bash
Xvfb :99 -screen 0 1280x720x24 >/tmp/opal-xvfb.log 2>&1 & XVFB=$!
DISPLAY=:99 make clean test
kill "$XVFB"
```

Expected: full suite passes; new codes are one-token; legacy codes are readable for migration; runtime has no relayed/TCP video or ffplay path.

- [ ] **Step 5: Commit**

```bash
git add include/opal/tunnel.hpp src/tunnel.cpp include/opal/tunnel_access.hpp src/tunnel_access.cpp src/zrok_cleanup.cpp include/opal/session.hpp src/session.cpp include/opal/host.hpp src/host.cpp src/client.cpp src/setup.cpp src/media.cpp include/opal/media.hpp tests/test_tunnel.cpp tests/test_tunnel_recovery.cpp tests/test_setup.cpp tests/test_session.cpp tests/test_media.cpp tests/test_daemon.cpp tests/test_hardening.cpp tests/integration.sh tests/smoke.sh src/system.cpp README.md Makefile
git commit -m "refactor: remove relayed OPAL video path"
```

---

### Task 12: Stress direct media loss/rekey behavior and verify the exact shipping SHA

**Files:**
- Create: `tests/test_direct_video_stress.cpp`
- Modify: `Makefile`
- Modify: `.github/workflows/ci.yml`
- Review: all files changed by Tasks 1-11

**Interfaces:**
- No new production public interfaces.
- Stress harness uses packet-send/receive injection hooks already present in `VideoSender`/`VideoReceiver` internals to simulate loss/reordering without changing production policy.

- [ ] **Step 1: Write the stress regression**

Run at least 50 deterministic generations with seeded packet perturbation. For each generation:

```text
send 120 synthetic video frames
randomly drop 0-5% ordinary data packets
randomly reorder adjacent packets
recover single-loss FEC groups
force at least one two-fragment loss requiring IDR recovery
force one control-generation replacement
inject one old-generation authenticated packet after rekey and verify rejection
verify sender queue <=2 frames and <=16 MiB
verify receiver state <=3 frames and <=16 MiB
verify at least one fresh frame is presented after every recovery
verify no stale ordinary packet is retransmitted
```

- [ ] **Step 2: Run and verify RED if the stress test exposes a defect**

```bash
make test-direct-video-stress
```

If the new test passes immediately, record that as evidence that existing implementations already satisfy the new stress coverage and do not manufacture a failure. If it fails, preserve the exact failing seed/output and fix only the demonstrated production defect before proceeding.

- [ ] **Step 3: Add the stress target to CI**

Update the `stress` job to run:

```bash
make all test-net test-session test-direct-video-session test-direct-video-pipeline test-direct-video-stress test-tunnel-recovery
for i in 1 2 3; do
  make test-session test-direct-video-stress
 done
```

Update ASan/UBSan to include packet, crypto, reassembly, capture, decoder, feedback, and stress targets. Keep TSan focused on session/direct-media orchestration and shared sender/receiver state.

- [ ] **Step 4: Run full local verification**

```bash
Xvfb :99 -screen 0 1280x720x24 >/tmp/opal-xvfb.log 2>&1 & XVFB=$!
DISPLAY=:99 make clean test
kill "$XVFB"
```

Expected: every unit, sanitizer-capable target, smoke test, direct integration path, cleanup/install test, and stress test passes.

- [ ] **Step 5: Commit stress coverage if changed**

```bash
git add tests/test_direct_video_stress.cpp Makefile .github/workflows/ci.yml
git commit -m "test: stress direct UDP video recovery"
```

- [ ] **Step 6: Verify GitHub Actions on the exact `main` head**

Record:

```bash
git rev-parse HEAD
```

Then require the GitHub Actions `CI` run whose `head_sha` exactly equals that SHA to complete with:

```text
linux           success
sanitize        success
thread-sanitize success
stress          success
```

Do not claim completion from an older green run or from only a combined status summary.

- [ ] **Step 7: Final spec-coverage audit**

Confirm from the final tree and tests:

```text
plain opal = 1080p60
temporary mode/fps overrides only
direct UDP media only
5-second direct negotiation deadline
TLS-exporter-derived ChaCha20-Poly1305 media keys
1024-packet replay window
1200-byte datagram cap
250 ms IDR cadence
2-frame/16 MiB sender bound
3-frame/16 MiB receiver bound
<=10% XOR FEC policy
no stale P-frame retransmission
native libavcodec decode
X11/GLX newest-frame presentation
<=40 ms non-master audio queue
100 ms feedback cadence
latency/debug telemetry
control-generation rekey/re-negotiation
clear direct-UDP/STUN/probe/capture/decoder/path errors
no ffplay
no zrok video share/access
no TCP video listener/fallback
legacy two-token codes handled only for migration/cleanup
exact-head four-lane CI success
```
