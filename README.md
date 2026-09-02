# OPAL

Performance-first native-resolution Linux remote desktop.

OPAL is deliberately small: one public CLI, one internal `/dev/uinput` helper, TLS/Ed25519 authentication, native screen/audio capture through mature Linux tools, and optional zrok private tunnels when direct networking is not available.

## Install

Dependencies on Debian/Ubuntu:

```bash
sudo apt install build-essential libssl-dev libx11-dev ffmpeg
```

GPU Screen Recorder is strongly recommended for low-overhead hardware capture on Wayland/X11. FFmpeg X11 capture is the fallback. zrok/zrok2 is optional and only needed for OPAL's private tunnel mode.

```bash
git clone https://github.com/xt9y/OPAL.git
cd OPAL
make
sudo make install
opal init
```

## Host

```bash
opal host setup
opal host
```

`host setup` creates `~/.opal/host.ini`, an Ed25519 identity, and a TLS certificate, then prints the pairing password.

To start OPAL automatically in your user session:

```bash
opal host enable
```

On Wayland, install GPU Screen Recorder and grant the desktop portal capture permission when prompted. On X11, OPAL falls back to FFmpeg `x11grab` if GPU Screen Recorder is unavailable.

## Connect

```bash
opal connect 192.168.1.50
```

The first connection asks for the host's pairing password. OPAL pins the host TLS certificate and authorizes the client's Ed25519 public key. Later connections use a signed challenge; the pairing password is not needed again.

The viewer runs fullscreen at the transmitted native resolution. Keyboard, relative mouse movement, buttons, and wheel events are sent on the control TLS connection while video/audio uses a separate TLS connection.

Release the client-side keyboard/mouse grab with:

```text
Ctrl + Alt + Shift + Q
```

## Saved hosts

```bash
opal hosts add desktop 192.168.1.50 00:11:22:33:44:55
opal hosts list
opal connect desktop
```

State is under `~/.opal/`:

```text
~/.opal/
├── config.ini
├── host.ini
├── hosts.ini
├── identity.key
├── identity.pub
├── authorized_clients
├── tls.crt
├── tls.key
└── logs/
```

Private keys and configuration files are created with owner-only permissions.

## Wake-on-LAN

Store the host's MAC address and run:

```bash
opal wake desktop
```

For wake from outside the LAN, run a tiny OPAL bridge on an always-on machine inside the host network:

```bash
opal bridge setup --mac 00:11:22:33:44:55
opal bridge run
```

The bridge prints a 256-bit wake secret. Put its address and secret in the saved host section:

```ini
[desktop]
address=192.168.1.50
mac=00:11:22:33:44:55
wake_bridge=10.0.0.2:47992
wake_secret=<secret>
```

A remote wake request is challenge/HMAC authenticated before the bridge emits the LAN magic packet. The sleeping computer still performs normal OPAL authentication after it wakes. The bridge can itself be exposed through a private tunnel/VPN; do not expose unauthenticated UDP WoL to the Internet.

## zrok bridge/tunnel

OPAL does not require a paid relay or an `xt9y.de` backend. If direct IP/IPv6/router forwarding is unavailable, install zrok and run:

```bash
opal tunnel host
```

OPAL starts private TCP shares for control and video. Copy the two private share tokens and connect with:

```bash
opal connect 'zrok:CONTROL_TOKEN,VIDEO_TOKEN'
```

Both endpoints still use OPAL TLS and device authentication inside the tunnel. zrok is transport, not trust.

## Security model

- TLS 1.3 for control and media transport.
- Self-signed host certificate pinned on first successful pairing.
- First pairing requires the host password.
- Client identities use Ed25519 keys.
- Reconnects sign a fresh random challenge.
- Video session tokens are random, short-lived, and one-use.
- Input injection is isolated in `opal-input` through `/dev/uinput`.
- `opal-input` is not setuid. The installed udev rule uses session `uaccess`.
- Remote WoL bridge requests use a fresh challenge and HMAC-SHA256.

This is a pre-1.0 project. Do not expose OPAL ports directly to hostile networks unless you understand the current security model and keep the repository updated.

## Performance path

Preferred host path:

```text
Wayland/X11 → GPU Screen Recorder → hardware H.264 → FLV stream
                                              ↓
                                     TLS media socket
                                              ↓
                                    FFplay low-delay
```

Fallback:

```text
X11 → FFmpeg x11grab → x264 ultrafast/zerolatency → TLS → FFplay
```

Control traffic uses a different TLS connection so a saturated video socket does not intentionally queue keyboard/mouse events behind video data.

## Commands

```text
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

## Development

```bash
make
make test
```

The tests cover configuration, cryptographic primitives, WoL packet construction, capture command selection, CLI/setup smoke behavior, DESTDIR installation, first pairing, TLS certificate pinning, persisted authorization, and Ed25519 reconnect authentication.

Docs: https://xt9y.de/opal.html

## License

MIT
