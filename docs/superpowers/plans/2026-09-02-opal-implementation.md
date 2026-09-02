# OPAL Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Build and publish a compact Linux-first native remote desktop with authenticated remote control, native-resolution media, Wake-on-LAN, optional wake bridge, and optional zrok transport.

**Architecture:** `opal` owns configuration, identity, TLS protocol, host/client orchestration, wake, and tunnel lifecycle. GPU Screen Recorder/FFmpeg and FFplay provide mature media paths; `/dev/uinput` is isolated in `opal-input`.

**Tech Stack:** C++20, OpenSSL 3, X11/XWayland, Linux uinput, GPU Screen Recorder, FFmpeg/FFplay, optional zrok.

**Spec:** `docs/superpowers/specs/2026-09-02-opal-design.md`

## Global Constraints

- Keep the repository small and conventional.
- `make`, `make test`, `sudo make install`, and `make uninstall` are first-class.
- No mandatory paid backend or xt9y.de dependency.
- Separate media and control sockets.
- First pairing password; subsequent Ed25519 authentication.
- Remote wake never implies remote-control authorization.

### Task 1: Core state and crypto
- [x] Write config/crypto/WoL/media-command tests and observe failure.
- [x] Implement `~/.opal/` layout, INI persistence, Ed25519, HMAC, WoL packet generation, and capture command generation.
- [x] Run core tests to green.

### Task 2: TLS host/client protocol
- [x] Implement TLS 1.3 sockets, certificate creation/fingerprinting, pairing, authorization persistence, challenge signing, and one-use media tokens.
- [x] Add localhost pairing and reconnect integration test.
- [x] Fix concurrency/file-descriptor issues found by integration testing.

### Task 3: Media and control
- [x] Implement GPU Screen Recorder preferred capture and FFmpeg fallback.
- [x] Stream media over dedicated TLS socket into low-delay fullscreen FFplay.
- [x] Grab X11/XWayland keyboard/mouse and forward bounded control messages.
- [x] Inject validated events through internal Linux `/dev/uinput` helper.

### Task 4: Wake and tunnel
- [x] Implement local WoL.
- [x] Implement authenticated LAN wake bridge.
- [x] Implement optional zrok private control/video tunnel commands and client access mode.

### Task 5: Packaging, CI, documentation
- [x] Add systemd user units, udev rule, install/uninstall targets, DESTDIR test, CI, README, license, design and plan docs.
- [ ] Publish OPAL repository.
- [ ] Publish `xt9y.de/opal.html` and verify Vercel deployment.
