# OPAL Low-Latency + Rendezvous Migration Design

Date: 2026-09-04
Status: approved direction, implementation starts with performance instrumentation

## Goal

Turn OPAL into a self-contained remote-desktop system where an ordinary user installs OPAL only, pairs once, and then connects without installing or configuring zrok, Tailscale, ZeroTier, VPN software, port forwarding, IP addresses, STUN servers, or router/firewall rules.

Before replacing zrok, finish the low-latency media architecture so networking migration does not hide or compound media-path defects.

The final steady state is:

```text
                    OPAL rendezvous
CLIENT ----------------------------------------- HOST
   |             signaling/presence only          |
   |                                              |
   +========== direct encrypted session ==========+
          input/control/video/audio/telemetry

If and only if direct P2P is impossible:

CLIENT ===== E2E encrypted ===== OPAL relay ===== HOST
```

The relay never terminates OPAL application encryption and never becomes the normal path.

## Product invariants

1. Ordinary users install OPAL and nothing else.
2. No zrok binary, zrok account, zrok environment, zrok token, zrok share, or zrok runtime process remains after final cutover.
3. No permanent Tailscale, ZeroTier, VPN, manual port-forward, public-IP, or manual STUN configuration requirement.
4. Direct encrypted P2P is always preferred over relay.
5. Relay is used only after direct LAN/public/NAT-hole-punched paths fail.
6. No conventional playback/jitter queue is introduced to reduce packet loss or smoothness.
7. Freshness wins over perfect continuity for video.
8. Control/input must not depend on a high-latency TCP tunnel after P2P session establishment.
9. Pairing and device identity remain end-to-end authenticated; the rendezvous/relay service is not a trust anchor for session contents.
10. Debug telemetry must expose enough stage timing to distinguish capture, encode, packetization, sender pacing, network, kernel receive backlog, reassembly, decode, present, display cadence, and recovery events.
11. Debug mode never prints secrets, pairing codes, private keys, media keys, session tokens, or authentication proofs.
12. GitHub Actions remain manual-only; no commit-triggered workflow is introduced.

## Why the migration is ordered

Current tests show the direct UDP path itself can be much faster than the visible remote interaction while OPAL still spends excessive time in media processing. The current receiver also handles packet receive, reassembly, decode, and presentation on one thread, and the client currently software-decodes H.264 with one decoder thread. Replacing zrok first would make transport cleaner without fixing the dominant visible latency.

Therefore the migration has two major halves:

```text
A. MEDIA / INTERACTION LATENCY
   instrumentation
   cursor decoupling
   sender burst/GOP policy
   receive-path isolation
   decoder strategy
   presentation cadence
   bounded recovery

B. OPAL-NATIVE NETWORKING
   rendezvous protocol/service
   OPAL peer handshake and session keys
   direct control/input over UDP
   NAT traversal
   relay fallback
   zrok deletion
   final setup UX
```

Every stage has a testable cutover gate. zrok remains only as temporary scaffolding until the OPAL-native networking stages are proven, then it is deleted rather than retained as a hidden fallback.

# Part A: Low-Latency Media Architecture

## A0. Diagnostic telemetry first

`OPAL_DEBUG=1` becomes an end-to-end diagnostic mode, including remote host media telemetry delivered to the client over the authenticated control channel.

The client should eventually print roughly one compact sample per second:

```text
OPAL path direct candidate=srflx->srflx rtt=42.1ms
OPAL host capture=gsr encoder=h264 frame=48123B packets=47 send=2.8ms bitrate=28400kbps stale=0 chain=ok
OPAL client rx=47 kernel_drop=0 wire=23.5ms reassembly=3.2ms decode=4.7ms present=2.1ms fps=59.8
OPAL latency total=37.6ms loss=0.0% idr=0 audio=12ms
```

Minimum telemetry fields:

### Host

- capture backend
- encoded media type
- active stream width/height/FPS
- encoded frame bytes
- fragment count and FEC fragment count
- sender frame ID
- packetization time
- first-to-last send span
- target and active bitrate
- capture-to-packet age
- stale/drop count
- H.264 reference-chain state
- requested/restarted IDR count

### Client

- direct path candidate class
- RTT and clock-sync validity
- packets received per interval
- sequence loss
- kernel socket overflow/drop counter where Linux exposes it
- receive socket pending/backlog indicator where available
- first-to-last fragment arrival span
- reassembly time
- decoder backend
- decode time
- corrupt-frame count
- present time
- actual decoded FPS
- actual presented FPS
- stale/recovery count
- audio queued milliseconds

Debug traffic is bounded to a small fixed rate and never gates media delivery.

## A1. Local cursor feedback

The host capture stream must not include the remote cursor in the normal interactive path. GPU Screen Recorder and the FFmpeg fallback should capture the desktop without the cursor.

The client keeps its local native cursor visible over the presentation window and sends its absolute normalized pointer position to the host exactly as today.

Result:

```text
local physical pointer -> local visible cursor: local-display latency
local physical pointer -> host pointer: control/network latency
host content response -> video: media latency
```

This prevents media delay from making the cursor itself look 100-200 ms behind.

## A2. GOP and sender serialization policy

The current short fixed IDR cadence is too expensive for a latency-first stream because large keyframes consume significant sender serialization time.

Normal policy becomes:

- startup/recovery always begins from a clean IDR;
- normal GOP is materially longer than 250 ms, initially 2 seconds;
- explicit `REQUEST_IDR` remains immediate;
- capture restart always re-establishes a clean IDR/config boundary;
- no P-frame is emitted after a sender-known reference drop until a clean IDR succeeds.

Sender pacing becomes frame-aware instead of a two-packet token bucket. Average path bitrate remains bounded, but one useful encoded access unit gets enough burst allowance to avoid stretching ordinary frames across many frame intervals.

Initial burst target:

```text
max(128 KiB, 2 * average_encoded_frame_bytes_at_target_bitrate)
```

with a hard bounded ceiling. Keyframes may use a larger bounded startup/recovery allowance, but the implementation must not create an unbounded kernel/network queue.

The metric that matters is `first packet send -> last packet send` for each access unit. At 60 FPS, ordinary-frame serialization should normally remain below one frame interval.

## A3. Receive thread isolation

The UDP receive/reassembly path must never block on H.264 decode or OpenGL presentation.

Split the client into:

```text
RX thread
  poll/batch receive
  authenticate/decrypt
  replay check
  reassembly/FEC
  complete access-unit handoff

media thread
  decoder
  presenter
```

There is no conventional playback queue.

The handoff is bounded and reference-aware. If the decoder cannot keep up, OPAL does not accumulate frames behind real time. Instead it invalidates the current dependent chain, requests an IDR, and resumes from a fresh keyframe. This preserves the low-latency live-edge policy.

The receive side should use `recvmmsg()` on Linux where available to drain bursts efficiently, with the existing portable single-datagram path retained as a bounded fallback implementation detail.

## A4. Decoder strategy

The current one-thread software decoder is not a valid 1080p60 performance target when measured decode time exceeds a frame interval.

Decoder selection becomes explicit and visible in debug telemetry.

Order:

1. low-latency hardware decoder when FFmpeg exposes a usable backend for the platform/device;
2. low-latency software configuration validated not to create an unacceptable reorder delay;
3. if the decoder cannot sustain requested FPS, OPAL must prefer live-edge freshness over queue growth.

Hardware decoding is an implementation optimization, not a different network architecture. The first implementation should detect FFmpeg-supported hardware device types at runtime and choose only backends that successfully initialize for H.264.

The software fallback must no longer hard-code `thread_count=1` without evidence that it meets the requested stream profile.

## A5. Presentation path

The GL presenter remains OPAL-owned and queue-free.

Requirements:

- swap interval disabled when supported;
- no extra CPU RGB conversion in the normal YUV/NV12 path;
- presentation timing included in telemetry;
- actual presented FPS counted;
- no retained frame queue beyond the currently presented frame and bounded decoder handoff state;
- compositor bypass remains best-effort on X11/XWayland.

## A6. Performance gate

Before networking migration begins, OPAL must be able to produce debug evidence that identifies every major stage.

Target on a healthy 1080p60 path whose measured one-way network component is about 20-25 ms:

```text
capture/encode       <= 8 ms typical
sender serialization <= 8 ms ordinary frame typical
network              path dependent
reassembly           <= 5 ms typical after last-fragment accounting is corrected
video decode         <= 10 ms typical
present              <= 5 ms typical
```

The target is approximately 35-60 ms glass-to-glass on a path with ~20-25 ms one-way propagation. These are engineering targets, not correctness assertions; debug telemetry decides what is optimized next.

# Part B: OPAL-Native Networking

## B0. Architecture choice

Use an OPAL-owned rendezvous service plus the existing direct UDP philosophy. The service has two deployable roles:

```text
rendezvous: presence + session introduction + candidate exchange
relay:      blind encrypted datagram forwarding when direct P2P fails
```

The official OPAL client defaults to the project-operated endpoint. The same server implementation is self-hostable through configuration for development/private deployments, but ordinary users never need to configure or understand it.

This is preferred over embedding zrok/Tailscale/ZeroTier because those products create user accounts, external state, extra daemons, and product coupling. It is preferred over direct-only networking because direct-only cannot work across every symmetric NAT/CGNAT combination.

## B1. Connection code and device identity

A host setup creates a stable random public rendezvous identifier that is not an IP address and contains no secret session material.

User-facing code format becomes short and human-shareable, for example:

```text
opal:K7F4-D92Q
```

Internally the code resolves to a stable host rendezvous ID. Pairing still requires the OPAL pairing proof and pins the host identity. Possession of the connection code alone is not authorization.

After pairing, the client stores:

- host rendezvous ID
- host public identity/fingerprint
- client device identity
- friendly host name
- optional learned Wake-on-LAN metadata

The connection code is not a bearer token for desktop access.

## B2. Rendezvous service responsibilities

The rendezvous service may know:

- opaque device/rendezvous IDs
- online/offline presence with short expiration
- temporary session introduction IDs
- source-observed public endpoint metadata needed for NAT traversal
- authenticated candidate blobs
- relay allocation IDs and bandwidth accounting

It must not receive:

- desktop frames in direct mode
- plaintext input/control messages
- pairing passwords
- long-term private keys
- media/session traffic keys

Host presence is lease-based and expires automatically if the host disappears.

## B3. Peer authentication and key agreement

The current media keys depend on a TLS exporter from the peer-to-peer control TLS connection. Removing zrok means that key derivation must no longer depend on a tunneled TCP/TLS peer connection.

The OPAL-native session handshake uses device identities plus ephemeral ECDH:

1. each side creates an ephemeral X25519 keypair for the connection generation;
2. each side signs its ephemeral public key, generation, peer identity, and rendezvous session nonce with its long-term Ed25519 device identity;
3. pairing-time authorization determines whether that device identity is trusted;
4. X25519 shared secret + transcript is fed through HKDF-SHA256;
5. derive independent direction keys/nonces for control, media, probe, and relay envelopes;
6. erase ephemeral private/session key material on generation teardown.

The rendezvous service forwards handshake messages but cannot derive the shared secret.

First pairing uses the existing password-based proof concept, bound to both long-term identities and the rendezvous session transcript. Subsequent sessions use signatures only.

## B4. Unified direct UDP session

After rendezvous introduction, peers gather candidates and perform authenticated UDP hole punching.

Candidate priority:

1. same-LAN/local candidates;
2. source-observed/server-reflexive public candidates;
3. additional STUN-discovered candidates only if useful;
4. relay allocation only after direct deadline fails.

The rendezvous service can supply source-observed public endpoint information, reducing the need for users or client configuration to know anything about STUN.

After a direct path is selected, normal OPAL traffic uses one session abstraction over UDP:

```text
unreliable latest-wins:
  pointer position
  ordinary video
  audio where freshness is preferable
  telemetry

reliable ordered/bounded:
  key/button transitions
  pairing/auth control
  stream control
  IDR request
  generation changes
  configuration
```

Reliable control is implemented at the OPAL session layer with sequence IDs, acknowledgements, bounded retransmission, and duplicate suppression. It must not become an unbounded general-purpose TCP replacement.

Pointer motion remains latest-wins and should never wait behind reliable control retransmissions.

## B5. Relay fallback

If direct authenticated UDP cannot be established inside the direct-path deadline, both peers request a short-lived relay allocation from the OPAL service.

Relay packet shape remains end-to-end encrypted between client and host. The relay only validates an outer allocation/session routing token and forwards bounded datagrams.

Relay properties:

- never normal/default when a direct path works;
- no media decryption;
- short-lived allocations;
- bandwidth/rate limits;
- anti-amplification checks;
- session-level authorization before allocation;
- no arbitrary Internet forwarding;
- debug mode clearly reports `path=relay` rather than hiding it.

Relay can increase latency, but it prioritizes connectivity over the current direct-or-fail behavior.

## B6. Rendezvous transport

Rendezvous traffic is tiny compared with media. The first implementation should prioritize deployability and security over inventing another transport protocol.

Use normal TLS to the rendezvous service for registration/introduction messages. The exact wire framing may be a compact HTTP/JSON or length-bounded binary request/response API during the first deployment; this path is not latency-critical after session establishment.

Normal peer traffic leaves the rendezvous TLS connection as soon as direct/relay session establishment succeeds.

## B7. Host service behavior

`opal-host.service` starts without zrok and maintains a renewable rendezvous presence lease.

The host listens on its OPAL UDP socket and receives authenticated introductions through the rendezvous client.

A sleeping/offline host is represented as offline by lease expiry. Wake-on-LAN remains a separate optional capability where the client has a reachable LAN/bridge route; rendezvous does not pretend to wake a powered-down machine through NAT by itself.

## B8. Final setup UX

Host:

```text
$ opal setup
OPAL host ready.
Connection code: opal:K7F4-D92Q
```

Client first pairing:

```text
$ opal
OPAL connection code: opal:K7F4-D92Q
Save as [desktop]:
Pairing password: ....
Connected.
```

Later:

```text
$ opal
Waking desktop...
Connecting...
Connected.
```

No output mentions rendezvous, relay, NAT, STUN, zrok, port forwarding, VPNs, or public IPs during a normal successful connection. `OPAL_DEBUG=1` may show these implementation details.

## B9. zrok deletion gate

Do not delete zrok at the beginning. Delete it only when all of these are true:

1. official/self-hosted rendezvous server implementation exists;
2. host can register and maintain presence without zrok;
3. client can resolve a saved host through rendezvous;
4. first pairing and saved-identity reconnect work through OPAL-native introduction;
5. direct UDP session carries both media and control/input;
6. control recovery creates a fresh OPAL-native generation without zrok;
7. relay fallback works under a forced-direct-failure integration test;
8. setup/install/doctor/clean work on a machine with no `zrok2` executable;
9. README/install docs contain no zrok setup requirement;
10. hardening test asserts production source contains no zrok runtime dependency.

Then delete:

```text
src/tunnel.cpp
src/tunnel_access.cpp
src/tunnel_supervisor.cpp
src/zrok_cleanup.cpp
include/opal/tunnel.hpp
include/opal/tunnel_access.hpp
include/opal/tunnel_supervisor.hpp
all zrok tests
all zrok configuration keys
all legacy zrok cleanup/runtime code
all zrok README/install instructions
```

Old saved host configurations containing `opal:CONTROL_TOKEN` are either migrated through an explicit one-time compatibility command before the cutover release or rejected with a clear pre-1.0 upgrade message. No zrok executable is invoked to migrate them after the final cutover.

# Component boundaries

Keep files focused.

## Performance/media

```text
media_profile.*        stream/GOP/burst policy
media_debug.*          structured bounded telemetry and parsers
video_capture.*        capture/demux timestamps/backend metadata
video_sender.*         packetization/pacing/reference state
udp_transport.*        sockets, batch RX, kernel-drop metadata
video_reassembly.*     fragment/FEC/reference state
video_decoder.*        backend selection/decode
video_present.*        GL presentation/FPS
video_receiver.*       orchestration of RX + media worker
```

## OPAL-native networking

```text
rendezvous_protocol.*  bounded messages/types
rendezvous_client.*    host presence + client introductions
peer_handshake.*       Ed25519/X25519/HKDF transcript
session_packet.*       direct-session control/media envelope
reliable_control.*     ACK/retry/dedup for critical control
peer_session.*         selected direct/relay path lifecycle
relay_protocol.*       relay allocation/outer routing envelope
server/rendezvous.*    service implementation
server/relay.*         blind forwarding implementation
session.*              product orchestration, not transport internals
```

Do not fold rendezvous, reliability, NAT traversal, crypto, and media into `session.cpp`.

# Failure behavior

User-facing normal errors are concise:

```text
OPAL host is offline.
Could not reach OPAL service.
Could not establish OPAL session.
Authentication failed.
```

Debug mode may add precise causes:

```text
rendezvous lease expired
candidate direct deadline exceeded
relay allocation denied
peer signature invalid
control ACK timeout
kernel RX overflow
video decoder behind live edge
```

The service being unavailable must not break already-established direct sessions.

# Security requirements

- Strict input length limits on every server and peer message.
- Anti-replay sequence spaces per generation/direction.
- AEAD for all peer/relay session datagrams.
- Long-term private keys never leave the device.
- Ephemeral session private keys never persist to disk.
- Relay cannot derive application keys.
- Rendezvous cannot authorize a client merely because it knows a host code.
- Pairing proof is bound to host identity, client identity, ephemeral handshake, and session nonce.
- Generation changes derive fresh keys.
- Old-generation packets are rejected.
- Relay allocations are short-lived and scoped to one authenticated pair/session.
- Server endpoints implement request size/rate limits and anti-amplification behavior.

# Testing strategy

All production behavior is introduced TDD-first.

Minimum test families:

## Performance

- media debug line formatting/parsing and secret exclusion
- sender ordinary-frame burst budget
- longer normal GOP command generation
- capture cursor disabled
- reference-chain invalidation on stale/drop
- RX worker continues draining while decode worker is blocked in a synthetic test
- bounded handoff triggers IDR instead of queue growth
- decoder backend selection fallback
- presented-FPS counters
- kernel RX overflow counter parsing where supported

## Rendezvous

- connection-code validation
- presence lease expiry
- host/client introduction authorization
- source-observed endpoint handling
- malformed/oversized protocol messages
- replayed introduction rejection

## Peer handshake

- Ed25519 identity verification
- X25519 shared-secret agreement
- transcript mismatch rejection
- old-generation rejection
- pairing proof binding

## Session control

- reliable key/button retransmission/ACK
- duplicate suppression
- pointer latest-wins/coalescing
- control retransmission never blocks media/pointer path

## NAT/relay

- LAN direct selection
- public direct selection
- forced direct failure -> relay
- relay ciphertext remains opaque to relay implementation
- relay rate/allocation limits

## Cutover

- full install/test on system without zrok
- source/README grep rejects `zrok2` runtime dependency after final deletion
- manual-only CI invariant remains

# Stage order

```text
0  Commit this architecture + implementation plan
1  End-to-end OPAL_DEBUG media telemetry
2  Local cursor + longer GOP + frame-aware sender burst
3  RX/media thread separation + batch receive
4  Decoder backend/throughput improvements
5  Presentation/FPS and live-edge recovery tuning
6  Performance validation gate
7  Rendezvous protocol + self-hostable server
8  Host presence + short connection codes
9  Peer E2E handshake independent of TLS exporter
10 Unified direct UDP control/input session
11 NAT traversal through rendezvous-observed candidates
12 Relay fallback
13 Session recovery/generation migration
14 Setup/install/doctor/clean migration
15 Delete all zrok code/config/docs/tests
16 Final security/stress/install validation
```

No stage advances merely because it compiles. Each stage requires its targeted tests and, where applicable, a real `OPAL_DEBUG=1` run on the user's machines before latency assumptions are accepted.