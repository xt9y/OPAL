# OPAL

OPAL is a performance-first Linux remote desktop with a native client/host CLI, direct end-to-end encrypted UDP media, low-latency input, automatic NAT traversal, and blind relay fallback.

## Normal use

Build and install OPAL on both Linux computers:

```bash
make -j"$(nproc)"
sudo make install
```

Run:

```bash
opal
```

On the host, choose **Host this computer**. OPAL creates its long-term identity and prints a public connection code:

```text
OPAL host ready.
Connection code: opal:XXXX-XXXX-XXXX
Pairing password: XXXX-XXXX-XXXX-XXXX
```

On the client, choose **Connect to another computer**, enter the connection code, name the host, and enter the one-time pairing password when requested. The client pins the host Ed25519 identity after pairing. Future connections use the saved identity and do not require the pairing password.

No separate VPN, account, networking CLI, router configuration, public IP, port forwarding, or manual STUN configuration is part of the normal OPAL setup.

## Networking

OPAL uses a small public rendezvous service only to introduce peers and observe their public UDP mappings. The rendezvous service is not the session security boundary.

```text
                    OPAL rendezvous
CLIENT  -------------------------------  HOST
   |                                         |
   +====== direct encrypted UDP session =====+
           input / control / video / audio
```

Each connection generation uses fresh X25519 ephemeral keys. Long-term Ed25519 identities sign the handshake transcript, and HKDF-SHA256 derives independent directional keys for control, media, probes, relay transport, and key confirmation.

OPAL first attempts the server-observed direct UDP peer path. If the direct handshake cannot be established, both peers may use a short-lived authenticated relay allocation:

```text
CLIENT === opaque OPAL ciphertext === RELAY === opaque OPAL ciphertext === HOST
```

The relay only reads a small routing envelope. It forwards the unchanged inner OPAL ciphertext and does not receive peer session keys.

The public connection code (`opal:XXXX-XXXX-XXXX`) is a checksummed locator derived from the host public identity. It is not a password or session secret.

## Latency design

OPAL is intentionally live-edge rather than playback-oriented:

- pointer updates are latest-wins and independent from reliable keyboard/button/config control;
- reliable control is bounded UDP retransmission rather than a general TCP-over-UDP stream;
- video does not use a growing playback/jitter queue;
- the receiver batch-drains UDP with `recvmmsg()` on Linux;
- packet reception/reassembly runs independently from decoding/presentation;
- the video handoff is capacity-one and requests a clean IDR instead of accumulating stale dependent frames;
- sender pacing permits bounded frame-sized bursts instead of serializing a useful frame over many display intervals;
- the host cursor is not baked into captured video;
- H.264 corruption/reference-chain gaps force a clean IDR boundary;
- decoder auto mode attempts FFmpeg-advertised hardware decoding and falls back to low-delay slice-threaded software decoding.

## Debugging latency

Run the client with:

```bash
OPAL_DEBUG=1 opal
```

Debug mode enables bounded host telemetry for that session and reports measurements such as:

```text
OPAL host frame=... bytes=... packets=...+... send=...ms capture->packet=...ms bitrate=...kbps target=...kbps stale=... idr=... restarts=... chain=...
OPAL latency capture->packet=...ms network=...ms reassembly=...ms decode=...ms present=...ms total=...ms loss=...% stale=... bitrate=...kbps decoder=... decoded=...fps presented=...fps audio=...ms rtt=...ms kernel_drop=...
OPAL connection path=direct
```

A fallback session reports `path=relay`. Debug output never includes identity private keys, peer session keys, pairing passwords, or authentication proofs.

Useful overrides:

```bash
OPAL_DECODER=software OPAL_DEBUG=1 opal
OPAL_RENDEZVOUS_HOST=host.example OPAL_RENDEZVOUS_PORT=47992 opal
```

## Stream overrides

```bash
opal --mode max
opal --mode 1440p --fps 120
```

Supported resolution modes are `max`, `1080p`, `1440p`, and `4k`; FPS is clamped to 15–240. Overrides apply only to the current connection.

## Requirements

Run:

```bash
opal doctor
```

The build requires a C++20 compiler, Make, pkg-config, OpenSSL development files, X11/XInput2/OpenGL development files, PulseAudio development files, and FFmpeg development libraries. GPU Screen Recorder is preferred for host capture when available; FFmpeg is the fallback capture path.

On Fedora-family systems, the dependency preflight prints the appropriate package hint:

```bash
make deps-check
```

## Self-host the rendezvous + relay service

The ordinary desktop install does not install the server daemon. Build it explicitly:

```bash
make rendezvous-server
```

The binary is:

```text
build/opal-rendezvous
```

Run it on a public UDP-capable Linux server:

```bash
OPAL_RENDEZVOUS_BIND=:: \
OPAL_RENDEZVOUS_PUBLIC_HOST=opal.example.org \
build/opal-rendezvous
```

Default port: UDP `47992`.

To install the server binary explicitly:

```bash
sudo make install-rendezvous
```

Desktop clients can point at a self-hosted deployment with `OPAL_RENDEZVOUS_HOST` and `OPAL_RENDEZVOUS_PORT`. Normal users use OPAL's compiled default endpoint and do not configure this manually.

## Commands

```text
opal
opal select
opal new
opal remove
opal restart
opal clean
opal doctor
opal version
opal help
```

Release remote control with `Ctrl+Alt+Shift+Q`.

Configuration and identities live under `~/.opal/` by default, or `OPAL_HOME` for tests.

## Development

Focused tests:

```bash
make test-rendezvous-protocol
make test-rendezvous-server
make test-peer-handshake
make test-session-packet
make test-reliable-control
make test-peer-session
make test-relay
make test-direct-video-pipeline
```

Full local gate:

```bash
make test
```

CI is intentionally manual-only.