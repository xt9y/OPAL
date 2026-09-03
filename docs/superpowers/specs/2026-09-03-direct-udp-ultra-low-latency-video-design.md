# OPAL Direct-UDP Ultra-Low-Latency Video Design

Date: 2026-09-03
Status: approved architecture, pending implementation plan

## Goal

Make OPAL video feel as close to a local display as practical while preserving OPAL's existing reliable keyboard/mouse/control behavior.

The default connection becomes a latency-first 1920x1080 ceiling at 60 FPS. Higher resolutions and frame rates remain temporary per-connection overrides.

The design intentionally prefers freshness over perfect continuity: a late video frame is worthless. OPAL must discard stale or incomplete video rather than buffer behind real time.

## Non-goals

- Literal zero latency. Capture cadence, encode/decode time, network propagation, and display scanout impose unavoidable delay.
- Reliable delivery of ordinary video frames.
- A zrok UDP video fallback.
- A TCP video fallback.
- Upscaling a host below the requested resolution ceiling.
- Persisting `--mode` or `--fps` overrides.
- Replacing the existing reliable control/input transport.

## Success criteria

On a healthy low-latency direct path, OPAL should target roughly 25-50 ms glass-to-glass at 1080p60, plus any unavoidable path-specific propagation delay outside OPAL's control.

The implementation is successful when:

1. Plain `opal` requests a 1920x1080 ceiling at 60 FPS.
2. `--mode max|1080p|1440p|4k` and `--fps 15-240` remain temporary per-connection overrides.
3. Video media no longer traverses the zrok/TCP video stream during normal operation.
4. Direct encrypted UDP is the only video transport.
5. Failure to establish direct UDP produces a clear connection error instead of falling back to a relay or TCP video path.
6. The receiver never accumulates an unbounded or conventional playback/jitter queue.
7. Old/incomplete frames are discarded when newer useful video is available.
8. Audio cannot delay video.
9. Loss recovery does not wait for stale P-frame retransmission.
10. Control, keyboard, and mouse remain on the existing authenticated reliable TLS/zrok channel.
11. OPAL exposes enough latency telemetry to identify capture, network, decode, and presentation delay separately.

## High-level architecture

```text
CONTROL / INPUT
client <==== TLS 1.3 over existing zrok private TCP ====> host

Used for:
- authentication and pairing
- keyboard/mouse/control
- UDP candidate exchange
- video session negotiation
- keyframe requests
- stream profile changes
- latency/control telemetry

VIDEO
host capture -> host media demux -> frame-aware packetizer
            -> ChaCha20-Poly1305 UDP datagrams
            -> direct Internet/LAN UDP
            -> frame reassembly -> libavcodec decode
            -> latest-frame presentation
```

There is deliberately no media fallback path after direct UDP.

## Default stream semantics

### Plain `opal`

Plain `opal` is equivalent to:

```text
--mode 1080p --fps 60
```

`StreamOptions` therefore defaults to:

```text
max_width  = 1920
max_height = 1080
fps        = 60
```

A host with a lower native resolution is never upscaled.

### Temporary overrides

The existing override model remains:

```text
opal --mode max
opal --mode 1080p
opal --mode 1440p
opal --mode 4k
opal --fps 120
opal --mode 1440p --fps 120
```

Overrides affect only that process/session. Nothing is persisted to `~/.opal` or the host configuration.

## Direct UDP establishment

The existing authenticated control session coordinates direct UDP setup.

### Candidate generation

Each peer opens one non-blocking UDP socket before media starts and gathers:

1. local interface candidates for direct LAN connectivity;
2. a server-reflexive public candidate obtained through STUN.

OPAL should support a small configurable STUN server list. The implementation must not depend on a media relay/TURN service. STUN is used only to discover the externally visible UDP mapping.

The control connection exchanges the candidate set after successful OPAL authentication.

### Hole punching

Both peers send authenticated UDP probes to all viable peer candidates in parallel. The first mutually authenticated path that succeeds becomes the selected media path.

LAN candidates should naturally win when both machines are on the same network because they complete first.

### Direct-only failure behavior

If no authenticated direct UDP path is established within the negotiation deadline, OPAL reports a specific error such as:

```text
Direct UDP video could not be established. This network/NAT does not permit OPAL's direct-only video path.
```

OPAL does not start zrok UDP, does not reopen the old TCP video stream, and does not hide the failure behind repeated reconnect messages.

This means some symmetric-NAT/CGNAT combinations may be unable to use OPAL video without network changes. That limitation is intentional under the approved direct-only policy.

## Session key derivation

Video datagrams receive independent OPAL authentication/encryption even though candidate negotiation uses the already encrypted control channel.

After the control TLS session is authenticated, both sides derive fresh per-session key material using TLS 1.3 exporter keying material from that exact control connection.

Conceptually:

```text
video_secret = TLS-Exporter(
    label   = "EXPORTER-OPAL-DIRECT-VIDEO-v1",
    context = session_id || client_identity || host_fingerprint
)
```

The exporter output is expanded into direction-specific keys and nonce bases:

```text
host_to_client_key
client_to_host_key
host_nonce_base
client_nonce_base
```

Keys are never saved to disk and die with the control session.

A new control generation therefore creates a new UDP video generation and fresh media keys.

## Datagram security

Each UDP datagram uses ChaCha20-Poly1305 through OpenSSL.

Reasons:

- authenticated encryption;
- efficient software performance on x86-64 and ARM64;
- no padding requirement;
- well suited to independent datagrams;
- already available through the OpenSSL dependency used by OPAL.

The packet sequence number participates in nonce construction and anti-replay validation. A packet sequence value must never repeat under the same direction key.

The receiver keeps a bounded replay window and rejects duplicate or too-old datagrams before frame processing.

## Datagram sizing

OPAL targets a maximum UDP payload size of approximately 1200 bytes, including OPAL's transport header and AEAD overhead.

This intentionally stays below common Internet MTUs and avoids relying on IP fragmentation.

No encoded access unit is sent as a single giant UDP datagram.

## Video packet format

All multibyte integer fields use network byte order.

A logical packet header contains:

```text
magic/version
session_id
direction/generation
packet_sequence
frame_id
capture_timestamp
media_type
frame_flags
fragment_index
fragment_count
payload_length
```

The authenticated-encryption tag follows the encrypted payload. Header fields needed for routing/reassembly are authenticated as AEAD additional authenticated data.

`media_type` initially distinguishes at least:

```text
VIDEO_H264
AUDIO_AAC
PROBE
PROBE_ACK
FEC
```

Important frame flags include:

```text
KEYFRAME
CONFIG
END_OF_ACCESS_UNIT
```

## Capture and host media path

GPU Screen Recorder remains the preferred capture/encode backend.

The default capture profile is latency-first:

```text
1920x1080 ceiling
60 FPS
H.264 hardware encode where available
CBR
performance-oriented encoder tuning
no B-frame dependency
short IDR cadence
```

The host must keep the current no-upscale behavior.

### Keyframe cadence

Target an IDR interval around 250-500 ms rather than one second. The final value should be chosen from measured bandwidth/recovery behavior, with latency and rapid corruption recovery prioritized over compression efficiency.

### Host demux boundary

The capture process may continue producing a stream container internally, but OPAL must not forward that container blindly over the network.

Instead the host media stage parses/demuxes the capture output and extracts timestamped H.264 access units and audio packets. Those media units become the boundary consumed by the direct-UDP packetizer.

This prevents client startup from depending on `ffplay`, stdin pipes, or live-container probing.

## Frame-aware packetization

Each H.264 access unit receives one monotonically increasing `frame_id` and is split into MTU-safe encrypted datagrams.

The receiver can therefore reason about complete/incomplete frames rather than receiving an opaque byte stream.

The packetizer must avoid creating large queues. If the sender is producing media faster than the socket/path can accept it, old unsent ordinary video data is discarded rather than allowed to accumulate behind real time.

Keyframe/configuration data receives higher retention priority than stale ordinary P-frame data.

## Loss and recovery policy

The policy is deliberately latency-first.

### Ordinary video loss

Do not retransmit stale ordinary P-frame fragments merely to preserve perfect continuity.

If a frame cannot be reconstructed before it becomes stale, discard it.

### Forward error correction

Add a small bounded FEC layer across fragments of an access unit or a small packet group. FEC must have a strict overhead cap and may never create a multi-frame waiting window.

The first implementation should favor simple XOR/parity-style recovery over a complex adaptive coding subsystem. More advanced FEC is only justified if measurements show it is needed.

### Decoder continuity loss

If loss makes the H.264 reference chain unusable:

1. discard affected/incomplete frames;
2. send `REQUEST_IDR` over the reliable control session;
3. discard dependent media until a usable IDR/config arrives;
4. resume immediately from that IDR.

Do not wait an RTT for retransmission of old frame data.

## Receiver reassembly policy

The receiver maintains only a very small bounded set of recent in-progress access units.

Core rule:

```text
frame N is incomplete
frame N+1/N+2 data proves N is no longer useful
=> discard N
```

Exact discard decisions must respect H.264 dependency state. OPAL may need to wait for/request an IDR after an unrecoverable reference loss, but it must not preserve stale data simply to keep temporal continuity.

A hard memory bound is required for all fragment/reassembly state.

## Native client decode path

The final low-latency path removes `ffplay` from normal video playback.

Client media path:

```text
UDP socket
  -> authenticate/decrypt
  -> replay check
  -> frame reassembly/FEC
  -> H.264 access unit
  -> libavcodec
  -> newest decoded frame
  -> direct OPAL presentation
```

### Decoder

Use FFmpeg/libavcodec directly with low-delay decoding behavior and no intentional reorder queue beyond what the selected H.264 stream strictly requires.

The encoder profile should avoid B-frames so decoder presentation does not require future-frame reordering.

Hardware decoding may be selected when initialization is fast and the backend is known-good. Software H.264 decode remains a valid fallback inside the client decoder because that is a decode implementation choice, not a network/media transport fallback.

### Presentation

OPAL owns the presentation surface instead of launching ffplay.

The presentation layer keeps at most one unpublished decoded video frame:

```text
new decoded frame arrives
old unpublished frame exists
=> replace/drop old unpublished frame
```

The renderer presents the newest useful frame as soon as the display API permits.

The implementation should preserve the current X11/XInput2 input-domain requirement so the video surface and input capture continue to operate in the same X11/XWayland domain where required.

## Audio policy

Audio is secondary to video latency.

Audio packets receive timestamps and sequence numbers but must never become the master clock for video.

If audio is late or its queue grows beyond a very small bounded threshold:

- drop stale audio;
- resynchronize audio to the current video timeline;
- never hold a video frame to recover A/V sync.

If maintaining low-delay audio materially destabilizes the first direct-UDP milestone, the implementation may stage video first and retain/disable audio temporarily behind an explicit development flag. The final architecture still requires low-delay audio over the same direct media path.

## Congestion and sender queue policy

A direct UDP sender cannot simply push a fixed 30-100 Mbps regardless of path capacity. Doing so would create kernel/network queues, packet loss, and latency.

The sender therefore needs latency-oriented pacing and simple congestion feedback.

Initial policy:

- use the requested resolution/FPS to choose an initial bitrate;
- monitor authenticated receiver feedback for loss and observed one-way/RTT trends;
- reduce bitrate quickly when queue/loss signals show the path is overloaded;
- increase bitrate conservatively when the path is healthy;
- never preserve quality by accumulating an encoder/network backlog.

If a backlog is detected, freshness wins: discard stale unsent media and/or lower bitrate.

Resolution and FPS do not automatically change in the first version unless the user explicitly selected them. Bitrate is the first adaptation mechanism.

## Timing and latency telemetry

Every video access unit carries a host capture/media timestamp. Control messages carry periodic clock-synchronization samples so OPAL can estimate offset between host and client monotonic clocks without using wall-clock time.

OPAL should expose debug telemetry such as:

```text
capture->packet       4.2 ms
network one-way      11.8 ms
reassembly            0.6 ms
decode                 3.1 ms
present wait           5.4 ms
estimated total       25.1 ms
loss                    0.3 %
dropped stale frames      2
bitrate                24 Mbps
```

Telemetry must not add a blocking round trip to frame delivery.

## Control protocol extensions

The authenticated reliable control protocol gains bounded messages for:

```text
UDP_CANDIDATE ...
UDP_PROBE_READY ...
UDP_SELECTED ...
REQUEST_IDR
VIDEO_FEEDBACK ...
CLOCK_SYNC ...
```

All control messages remain size-limited and strictly parsed.

No unauthenticated network input is allowed to trigger encoder configuration, keyframe generation, or persistent state changes.

## Control-generation behavior

The existing session supervisor treats a recovered control connection as a new generation. Direct UDP media must bind to that same generation.

When the control generation changes:

1. invalidate the old UDP session identifier;
2. erase old exporter-derived video keys;
3. close or reset the old media path;
4. derive new video key material from the new authenticated TLS connection;
5. renegotiate direct UDP candidates;
6. resume from a fresh keyframe.

Packets from an old generation are rejected even if they arrive later.

## Error handling

User-visible errors should distinguish:

```text
direct UDP negotiation failed
STUN discovery failed
authenticated UDP probe failed
video capture failed
video decoder failed
video path timed out
```

Normal direct-video loss should not produce the current repeated generic:

```text
Video interrupted; recovering...
Video stalled; reconnecting...
```

unless the actual media path is being renegotiated.

## Dependency changes

The native client media path adds direct build/link dependencies on the FFmpeg libraries required for demux/decode, expected to include:

```text
libavcodec
libavformat
libavutil
libswscale (if pixel conversion is required)
```

The final presentation backend should use the smallest dependency that preserves the current Linux/X11 input-domain behavior. SDL is acceptable only if it can be configured deterministically for that domain; otherwise a direct X11 presentation path is preferable.

OpenSSL remains responsible for TLS and UDP AEAD/key derivation.

## Repository/component boundaries

Keep responsibilities isolated rather than growing `session.cpp` or `host.cpp` into a monolith.

Recommended units:

```text
media_profile.*      stream defaults, bitrate/profile policy
udp_transport.*      UDP sockets, candidates, probes, selected path
video_crypto.*       exporter derivation, AEAD, replay protection
video_packet.*       packet format and strict parsing
video_reassembly.*   fragment/FEC/frame freshness logic
video_capture.*      capture process + host demux boundary
video_decoder.*      libavcodec access-unit decoder
video_present.*      latest-frame display
video_feedback.*     loss/latency/congestion feedback
session.*            orchestration only
```

Each unit must expose a bounded interface and have focused tests.

## Migration from the existing video path

Implementation should be staged so regressions are diagnosable, but the final architecture contains no old media fallback.

Suggested implementation milestones:

1. Change plain `opal` default to 1080p60 and add regression coverage.
2. Extract stream-profile logic from the current media implementation.
3. Add authenticated direct-UDP candidate/probe protocol while leaving it unused by production video.
4. Add video packet format, AEAD, replay protection, fragmentation, and reassembly tests.
5. Add host demux/access-unit output and native client decode/presentation behind test/development wiring.
6. Send real video through direct UDP in an integration path.
7. Add freshness/drop/FEC/keyframe-recovery behavior.
8. Add bitrate feedback/pacing and latency telemetry.
9. Move production `opal` to direct UDP video only.
10. Remove the old zrok/TCP video share/access path, ffplay production player, FLV-over-TLS forwarding, and obsolete tests/configuration.

The old path should not remain as hidden fallback after milestone 10.

## Testing strategy

### Unit tests

Cover at minimum:

- default 1080p60 profile;
- temporary override parsing;
- packet encode/decode and malformed packet rejection;
- nonce/sequence uniqueness;
- replay-window behavior;
- exporter key separation by generation/direction;
- fragmentation/reassembly boundaries;
- frame freshness/drop decisions;
- bounded memory behavior under fragment floods;
- FEC recovery and failure;
- IDR request state transitions;
- feedback/bitrate decisions;
- clock-offset calculations.

### Integration tests

Use loopback UDP with controlled loss, reordering, duplication, delay, and bandwidth constraints to verify:

- no stale playback queue forms;
- newer media replaces stale incomplete work;
- loss does not cause TCP-style head-of-line stalls;
- control recovery invalidates the previous video generation;
- direct-video failure reports a bounded clear error;
- keyframe recovery resumes decoding;
- input/control remains responsive while video is degraded.

### Sanitizers and stress

Continue requiring:

- normal Linux suite;
- ASan/UBSan;
- TSan;
- reconnect/transport stress.

Add long-running randomized UDP packet-order/loss stress with strict memory bounds.

### Real-system latency validation

CI cannot validate glass-to-glass latency. Add an OPAL debug latency mode and perform real host/client measurements after each media milestone.

The implementation must be optimized from those measurements rather than assumed to be low latency because a particular flag or transport was selected.

## Security considerations

- Direct UDP packets are accepted only after the authenticated control session derives the media keys.
- Every datagram is AEAD-authenticated before parsing media payload.
- Replayed packets are rejected.
- Candidate messages are accepted only over authenticated control.
- UDP probe responses prove possession of the per-session media key.
- Session IDs/generations prevent cross-session packet injection.
- Strict packet/header/fragment bounds prevent memory amplification.
- STUN responses are discovery input only and never establish OPAL trust.
- No media key is persisted.

## Performance principles

1. Freshness over continuity.
2. Drop rather than queue.
3. Never make video wait for audio.
4. Never retransmit stale ordinary frames.
5. Prefer direct LAN/Internet UDP over relayed media.
6. Keep control reliable and independent from media loss.
7. Bound every queue, fragment map, and recovery window.
8. Measure latency stage-by-stage before optimizing further.

## Final intended user experience

Normal use:

```text
opal
```

means direct encrypted 1080p60 latency-first video plus reliable OPAL control/input.

Power-user examples remain:

```text
opal --mode max
opal --mode 1440p --fps 120
opal --mode 4k --fps 60
```

If direct video cannot be established, OPAL says why and stops the video connection. It does not relay the video through zrok or switch back to TCP.