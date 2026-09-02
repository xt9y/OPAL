# OPAL

[![CI](https://github.com/xt9y/OPAL/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/xt9y/OPAL/actions/workflows/ci.yml?query=branch%3Amain)

Native Linux remote desktop.

- Native-resolution fullscreen desktop mirroring and remote control.
- TLS 1.3 transport with Ed25519 device authentication.
- GPU Screen Recorder preferred, FFmpeg fallback.
- Separate control and media connections for low input latency.
- Wake-on-LAN and authenticated remote wake bridge.
- Optional zrok private tunnels when direct networking is unavailable.
- Configuration and saved hosts under `~/.opal/`.

## Commands

```bash
opal
opal init
opal host
opal host setup
opal host enable
opal host disable
opal connect <host>
opal hosts list
opal hosts add <name> <address> [mac]
opal wake <host>
opal bridge setup --mac <mac>
opal bridge run
opal tunnel host
opal doctor
opal version
```

## Install

```bash
git clone https://github.com/xt9y/OPAL.git
cd OPAL
make
sudo make install
```

Then:

```bash
opal init
opal doctor
opal host setup
opal host
```

On another machine:

```bash
opal connect <host-ip>
```

Docs (Thanks to AI): https://xt9y.de/opal.html

## Notes

- OPAL is currently Linux-first and pre-1.0.
- GPU Screen Recorder is recommended for the fastest Wayland/X11 capture path.
- zrok is optional; OPAL does not require a paid relay or an `xt9y.de` backend.

## License

MIT
