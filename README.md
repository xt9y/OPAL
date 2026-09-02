# OPAL

[![CI](https://github.com/xt9y/OPAL/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/xt9y/OPAL/actions/workflows/ci.yml?query=branch%3Amain)

Native Linux remote desktop.

- Native-resolution fullscreen desktop mirroring and remote control.
- 60 FPS H.264/Matroska capture target with automatic video-session recovery.
- XInput2 keyboard/pointer capture with an X11 compatibility fallback.
- Linux uinput injection with full `KEY_MAX` support and stuck-input cleanup.
- TLS 1.3 transport with Ed25519 device authentication and certificate-bound first pairing.
- GPU Screen Recorder preferred, FFmpeg fallback.
- zrok2 private tunnels are the single normal network transport.
- Persistent systemd user host service that waits for clients in the background.
- Configuration under `~/.opal/`.

## Dependencies

Fedora / Fedora Asahi Remix:

```bash
sudo dnf install -y gcc-c++ make openssl-devel libX11-devel libXi-devel ffmpeg
```

Debian / Ubuntu:

```bash
sudo apt-get install -y g++ make libssl-dev libx11-dev libxi-dev ffmpeg
```

GPU Screen Recorder is recommended for Wayland capture. OPAL enables H.264 CPU fallback when a usable hardware encoder is unavailable.

## Install

```bash
git clone https://github.com/xt9y/OPAL.git
cd OPAL
make
sudo make install
systemctl --user daemon-reload
```

Install `zrok2` on both Linux computers with the universal installer. The temporary directory is private and is removed on exit, so another local user cannot substitute a predictable `/tmp` payload before the privileged install step:

```bash
set -euo pipefail

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

ARCH="$(uname -m)"
case "$ARCH" in
  x86_64|amd64) ARCH=amd64 ;;
  aarch64|arm64) ARCH=arm64 ;;
  armv7l|armv7) ARCH=armv7 ;;
  *) echo "Unsupported architecture: $ARCH"; exit 1 ;;
esac

VERSION="$(curl --proto '=https' --tlsv1.2 -fsSL https://api.github.com/repos/openziti/zrok/releases/latest | grep -m1 '"tag_name"' | cut -d '"' -f4 | sed 's/^v//')"
test -n "$VERSION"
curl --proto '=https' --tlsv1.2 -fL "https://github.com/openziti/zrok/releases/download/v${VERSION}/zrok_${VERSION}_linux_${ARCH}.tar.gz" -o "$tmpdir/zrok2.tar.gz"
tar --no-same-owner -xzf "$tmpdir/zrok2.tar.gz" -C "$tmpdir"
test -x "$tmpdir/zrok2"
sudo install -m 0755 "$tmpdir/zrok2" /usr/local/bin/zrok2
zrok2 version
```

Then just run:

```bash
opal
```

The first host setup creates two persistent private zrok2 shares and enables `opal-host.service`. The daemon binds only to loopback; zrok2 is the Internet-facing transport. `opal host` remains an explicit foreground/debug LAN listener.

Check the host service with:

```bash
systemctl --user status opal-host.service
```

The first Wayland capture can display the desktop portal screen-selection prompt. OPAL stores the GPU Screen Recorder portal-session token under `~/.opal/` and attempts to reuse it for later/recovered video sessions. If the compositor rejects the saved portal session, OPAL discards that token and allows a fresh screen selection.

The first host setup prints an OPAL connection code and a high-entropy pairing password. Enter the connection code on the client. After a successful pairing, OPAL rotates the host pairing password and later connections use the saved Ed25519 identity plus the pinned host certificate fingerprint.

If zrok2 is installed but not enabled, OPAL asks for your zrok enable token and runs `zrok2 enable` for you.

During a connection, OPAL keeps the authenticated control session separate from the replaceable video/player session. If capture, transport, or FFplay stalls, normal output reports:

```text
Video stalled; reconnecting...
Video restored.
```

and OPAL reconnects video without re-pairing or stopping the host.

Release remote keyboard/mouse control with:

```text
Ctrl+Alt+Shift+Q
```

For raw zrok2/GPU Screen Recorder/FFplay diagnostics:

```bash
OPAL_DEBUG=1 opal
```

Normal `opal` output intentionally suppresses those subprocess diagnostics.

## Clean local OPAL state

To completely reset OPAL and its zrok tunnels on the current computer:

```bash
opal clean
```

This stops and disables the local OPAL host/bridge services, terminates local zrok2 private share/access tunnel processes, deletes the persistent zrok shares created by this computer's OPAL host setup, and removes `~/.opal` including saved hosts, pairings, identities, connection codes, portal-session state, and OPAL configuration.

`opal clean` intentionally does **not** run `zrok2 disable` and does not remove `~/.zrok2`, so the zrok2 environment/login stays enabled. The next `opal` run starts as a fresh OPAL setup without requiring you to link/login to zrok again.

For advanced and scripting commands:

```bash
opal help
```

Docs (Thanks to AI): https://xt9y.de/opal.html

## Notes

- OPAL is currently Linux-first and pre-1.0.
- GPU Screen Recorder is recommended for the fastest Wayland/X11 capture path.
- zrok2 is required for normal networking.
- The normal daemon is loopback-only behind private zrok shares; direct foreground hosting is intentionally a separate debugging/LAN mode.

## License

MIT
