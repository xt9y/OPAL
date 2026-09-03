# OPAL

[![CI](https://github.com/xt9y/OPAL/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/xt9y/OPAL/actions/workflows/ci.yml)

Native Linux remote desktop focused on minimum interactive latency.

- Plain `opal` streams at a 1920x1080 ceiling and 60 FPS by default; lower-resolution hosts are never upscaled.
- Temporary `--mode` / `--fps` overrides support native, 1440p, 4K and 15-240 FPS operation.
- GPU Screen Recorder H.264/AAC capture is preferred; FFmpeg is the fallback capture backend.
- Video/audio media uses **direct encrypted UDP only**. There is no zrok-video or TCP-video fallback.
- zrok2 carries only the authenticated TLS 1.3 control/input connection.
- Media keys are derived from the authenticated TLS control generation and datagrams use ChaCha20-Poly1305.
- Frame-aware UDP packetization, bounded XOR FEC, stale-frame dropping and short IDR recovery keep the client near the live edge instead of buffering old video.
- Native libavcodec H.264 decode and an OPAL-owned X11/GLX presenter replace FFplay.
- Audio has a bounded <=40 ms queue and never gates video presentation.
- XInput2 keyboard/pointer capture with an X11 compatibility fallback.
- Linux uinput injection with full `KEY_MAX` support and stuck-input cleanup.
- Persistent systemd user host service that waits for clients in the background.
- Configuration under `~/.opal/`.

## Dependencies

`make` runs a dependency preflight before compiling the OPAL binary and uses `pkg-config` for FFmpeg include/library paths. This matters on Fedora, where FFmpeg headers are installed below `/usr/include/ffmpeg` rather than directly below `/usr/include`.

Fedora / Fedora Asahi Remix using Fedora's FFmpeg packages:

```bash
sudo dnf install -y \
  gcc-c++ make pkgconf-pkg-config openssl-devel libX11-devel libXi-devel \
  libglvnd-devel pulseaudio-libs-devel ffmpeg-free ffmpeg-free-devel
```

If the machine intentionally uses RPM Fusion's full FFmpeg packages, use `ffmpeg ffmpeg-devel` in place of `ffmpeg-free ffmpeg-free-devel`.

Debian / Ubuntu:

```bash
sudo apt-get install -y \
  g++ make pkg-config libssl-dev libx11-dev libxi-dev libgl1-mesa-dev \
  libpulse-dev ffmpeg libavformat-dev libavcodec-dev libavutil-dev \
  libswresample-dev
```

Arch Linux / CachyOS:

```bash
sudo pacman -S --needed \
  base-devel pkgconf openssl libx11 libxi libglvnd libpulse ffmpeg
```

GPU Screen Recorder is recommended for the lowest-latency Wayland/X11 capture path. OPAL enables its H.264 CPU fallback when a usable hardware encoder is unavailable.

## Install

```bash
git clone https://github.com/xt9y/OPAL.git
cd OPAL
make
sudo make install
systemctl --user daemon-reload
```

Install `zrok2` on both Linux computers with the universal installer. zrok is used for control only:

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

Then run:

```bash
opal
```

The first run opens OPAL setup. Host setup creates one persistent private zrok2 **control** share and enables `opal-host.service`. The host control listener remains loopback-only behind zrok. Media does not pass through zrok.

The host prints a connection code in this form:

```text
opal:CONTROL_TOKEN
```

Old `opal:CONTROL_TOKEN,VIDEO_TOKEN` codes remain readable only so OPAL can migrate/clean legacy resources. The legacy token is never used to carry media.

Check the host service with:

```bash
systemctl --user status opal-host.service
```

The first Wayland capture can display the desktop portal screen-selection prompt. OPAL stores the GPU Screen Recorder portal-session token under `~/.opal/` and attempts to reuse it for later/recovered capture generations.

The first client pairing uses the host pairing password. After successful pairing, OPAL rotates that password and later connections use the saved Ed25519 client identity plus the pinned host certificate fingerprint.

If zrok2 is installed but not enabled, OPAL asks for the zrok enable token and runs `zrok2 enable`.

## Direct video requirement

After the TLS control session authenticates, both peers gather local/STUN UDP candidates and attempt authenticated UDP hole punching. Direct video setup has a five-second deadline.

If the two networks cannot establish peer-to-peer UDP—for example some symmetric-NAT/CGNAT combinations—OPAL intentionally fails video with a direct-path error. It does **not** switch to relayed zrok video or ordered TCP video, because those fallbacks would reintroduce the latency architecture OPAL is designed to avoid.

Each recovered control generation negotiates a fresh UDP path and fresh TLS-exported media keys before media resumes.

## Stream profiles

Plain:

```bash
opal
```

means a 1920x1080 ceiling at 60 FPS.

Per-run overrides are not persisted:

```bash
opal --mode max
opal --mode 1440p
opal --mode 4k
opal --fps 120
opal --mode 1440p --fps 120
```

OPAL keeps at most bounded in-progress compressed state, does not maintain a conventional playback queue, and discards stale/incomplete ordinary video instead of drifting behind real time. IDR cadence is approximately 250 ms and packet loss can be repaired by bounded XOR FEC when exactly one fragment in a group is missing.

Release remote keyboard/mouse control with:

```text
Ctrl+Alt+Shift+Q
```

For direct-media diagnostics and latency telemetry:

```bash
OPAL_DEBUG=1 opal
```

Debug output reports media loss/bitrate and stage timing without changing the no-playback-clock presentation policy.

## Commands

```text
opal             Wake and connect to the selected host
opal select      Select a saved host
opal new         Run setup / add another host
opal remove      Remove a saved host
opal restart     Restart OPAL services
opal clean       Remove OPAL state and OPAL zrok control resources
opal doctor      Check local requirements
opal version     Show the version
opal help        Show command help
```

## Clean OPAL state

To completely reset OPAL and the zrok resources associated with it on the current computer:

```bash
opal clean
```

This stops local OPAL services, terminates OPAL-owned zrok private control processes, deletes the persistent control share, removes known legacy OPAL video shares when present, and removes `~/.opal` after remote cleanup is confirmed.

`opal clean` intentionally does **not** run `zrok2 disable` and does not remove `~/.zrok2`, so the global zrok environment/login stays enabled. If remote cleanup cannot be confirmed, OPAL preserves its local state so cleanup can be retried with the saved resource identifiers.

Docs: https://xt9y.de/opal.html

## Notes

- OPAL is Linux-first and pre-1.0.
- The native presenter currently targets X11/XWayland/GLX on the client.
- GPU Screen Recorder is recommended for the fastest capture path.
- zrok2 is required for normal control networking, not for video transport.
- Direct UDP reachability is mandatory for media by design.

## License

MIT
