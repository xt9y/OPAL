# OPAL

[![CI](https://github.com/xt9y/OPAL/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/xt9y/OPAL/actions/workflows/ci.yml?query=branch%3Amain)

Native Linux remote desktop.

- Native-resolution fullscreen desktop mirroring and remote control.
- TLS 1.3 transport with Ed25519 device authentication.
- GPU Screen Recorder preferred, FFmpeg fallback.
- Wake-on-LAN with automatic wake-before-connect.
- Optional authenticated wake bridge and zrok private tunnels.
- Configuration and saved hosts under `~/.opal/`.

## Install

```bash
git clone https://github.com/xt9y/OPAL.git
cd OPAL
make
sudo make install
```

Then just run:

```bash
opal
```

The first run asks whether this computer should host or connect. OPAL remembers that choice. A configured host starts hosting; a configured client connects to its default host and automatically sends Wake-on-LAN first when the host is offline and a MAC address is configured.

For advanced and scripting commands:

```bash
opal help
```

Docs (Thanks to AI): https://xt9y.de/opal.html

## Notes

- OPAL is currently Linux-first and pre-1.0.
- GPU Screen Recorder is recommended for the fastest Wayland/X11 capture path.
- zrok is optional; OPAL does not require a paid relay or an `xt9y.de` backend.

## License

MIT
