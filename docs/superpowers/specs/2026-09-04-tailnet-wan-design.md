# Zero-Cost Tailnet WAN Design

## Goal

Make normal `opal` work across arbitrary networks without an OPAL-owned VPS, domain, router port-forward, or changes to the latency-sensitive media transport.

## Architecture

Connection priority is LAN broadcast, saved Tailscale address, then the existing OPAL rendezvous fallback. A successful LAN session learns the host's `tailscale0` IPv4 address and stores it as a routing hint for later WAN sessions.

The Tailscale path reuses `discover_local_host()` with a unicast destination on UDP 47993. After the signed OPAL discovery exchange, the existing `PeerSession` and native OPAL UDP media path run unchanged. Tailscale is therefore an IP underlay only: it normally establishes a direct WireGuard/UDP path through NAT and uses DERP only when direct traversal is impossible.

## Automatic Tailscale bootstrap

Users do not install Tailscale separately. Normal OPAL startup checks for a working Tailscale IPv4 address. If Tailscale is missing, OPAL installs it with the official `curl -fsSL https://tailscale.com/install.sh | sh` installer. If Tailscale is installed but not connected, OPAL starts the service when systemd is available and runs `sudo tailscale up`.

Once `tailscale ip -4` succeeds, later OPAL starts do not redownload or reconfigure Tailscale. If installation or login fails, OPAL warns and continues so LAN/direct OPAL remains usable.

## Security

The saved tailnet address is not trusted as host identity. The existing OPAL rendezvous ID, signed discovery offer, expected host public key, pairing/authorization, session handshake, and media encryption remain authoritative. A stale or reassigned tailnet address cannot pass OPAL identity verification.

## Latency

No OPAL video is converted to TCP or a reliable byte stream. Packetization, 1200-byte datagrams, FEC, pacing, ChaCha20-Poly1305, live-edge dropping, decoding, and presentation are unchanged. The only extra latency on a direct tailnet path is the underlying Internet route plus WireGuard encapsulation. DERP is a connectivity fallback and may add an extra network hop.

## Bootstrap and failure behavior

OPAL automatically ensures Tailscale is installed and connected on interactive startup. While both machines are on the same LAN, one successful OPAL session lets the client learn and persist the host's 100.64.0.0/10 address. Later off-LAN sessions try that address automatically.

If `tailscale0` is absent, the saved address is invalid, or unicast discovery times out, OPAL falls through to its existing rendezvous path. LAN behavior is unchanged and Tailscale remains optional.
