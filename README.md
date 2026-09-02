# OPAL

[![CI](https://github.com/xt9y/OPAL/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/xt9y/OPAL/actions/workflows/ci.yml?query=branch%3Amain)

Native Linux remote desktop.

- Native-resolution fullscreen desktop mirroring and remote control.
- TLS 1.3 transport with Ed25519 device authentication.
- GPU Screen Recorder preferred, FFmpeg fallback.
- zrok2 private tunnels are the single network transport.
- No IP address, port forwarding, LAN detection, or relay fallback logic.
- Configuration under `~/.opal/`.

## Install

```bash
git clone https://github.com/xt9y/OPAL.git
cd OPAL
make
sudo make install
```

Install `zrok2` on both Linux computers with the universal installer:

```bash
ARCH="$(uname -m)"
case "$ARCH" in
  x86_64|amd64) ARCH=amd64 ;;
  aarch64|arm64) ARCH=arm64 ;;
  armv7l|armv7) ARCH=armv7 ;;
  *) echo "Unsupported architecture: $ARCH"; exit 1 ;;
esac

VERSION="$(curl -fsSL https://api.github.com/repos/openziti/zrok/releases/latest | grep -m1 '"tag_name"' | cut -d '"' -f4 | sed 's/^v//')"
curl -fL "https://github.com/openziti/zrok/releases/download/v${VERSION}/zrok_${VERSION}_linux_${ARCH}.tar.gz" -o /tmp/zrok2.tar.gz
tar -xzf /tmp/zrok2.tar.gz -C /tmp
sudo install -m 0755 /tmp/zrok2 /usr/local/bin/zrok2
zrok2 version
```

Then just run:

```bash
opal
```

The first host setup creates two persistent private zrok2 shares and prints one OPAL connection code. Enter that code on the client. OPAL remembers it and every later `opal` run uses the tunnel automatically.

If zrok2 is installed but not enabled, OPAL asks for your zrok enable token and runs `zrok2 enable` for you.

For advanced and scripting commands:

```bash
opal help
```

Docs (Thanks to AI): https://xt9y.de/opal.html

## Notes

- OPAL is currently Linux-first and pre-1.0.
- GPU Screen Recorder is recommended for the fastest Wayland/X11 capture path.
- zrok2 is required for networking.

## License

MIT
