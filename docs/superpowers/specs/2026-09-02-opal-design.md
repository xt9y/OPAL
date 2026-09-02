# OPAL Design

OPAL is a Linux-first native remote desktop optimized for native-resolution fullscreen use. The repository stays intentionally small and follows the installation conventions used by C-BuildSystem and RendererCheck: `make`, `make test`, `sudo make install`, one public CLI, focused source modules, no generated project scaffolding.

## Runtime

The host exposes separate TLS 1.3 control and media sockets. First connection authenticates with a short pairing password and persists the client's Ed25519 public key. Subsequent connections use a signed random challenge. The client pins the host certificate fingerprint after successful pairing. Media receives a one-use session token over the authenticated control channel.

Preferred capture is GPU Screen Recorder using hardware H.264 with audio; FFmpeg X11 capture is the fallback. The client feeds the stream into FFplay configured for low delay and grabs X11/XWayland input, forwarding evdev-compatible key codes and relative mouse events. The host passes validated control messages to an internal `/dev/uinput` helper.

## Networking

Direct IP/LAN/IPv6/router configuration remains the zero-service path. Optional zrok private TCP shares provide outbound-only tunnel transport for networks where direct reachability is unavailable. OPAL encryption/authentication remains active inside that tunnel. `xt9y.de` is documentation only and is not required for streaming.

## Wake

Same-LAN wake emits a normal WoL packet. Internet wake requires an always-on OPAL bridge on the target LAN. Bridge wake requests use a fresh random challenge and HMAC-SHA256 before the bridge emits the magic packet; a sleeping target never receives remote-control trust from the wake operation.
