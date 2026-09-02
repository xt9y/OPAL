# OPAL Security, Stability, and Latency Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate the audited security/stability failure modes and reduce control/video latency while preserving the existing OPAL CLI and zrok2 transport contract.

**Architecture:** Keep control and video isolated. Harden pre-auth networking and process lifecycle, bind pairing to the actual host certificate, make video tokens single-use, move live input to a bounded nonblocking writer, and make client tunnel instances session-owned with dynamic local ports.

**Tech Stack:** C++20, OpenSSL TLS 1.3/Ed25519/HMAC, Linux sockets/uinput, X11/XInput2, zrok2, GPU Screen Recorder/FFmpeg/FFplay, systemd, Make.

**Spec:** `docs/superpowers/specs/2026-09-02-security-stability-latency-hardening-design.md`

## Global Constraints

- Work directly on `main` as explicitly requested.
- Preserve zrok2 as the normal required Internet transport.
- Preserve separate control and video channels.
- Preserve Linux-only runtime and current CLI behavior.
- Every production behavior change starts with a regression test.
- Do not add a QUIC dependency in this hardening pass.

---

### Task 1: Network primitives and pre-auth deadlines

**Files:** modify `include/opal/net.hpp`, `src/net.cpp`; test `tests/test_net.cpp`.

- [ ] Add tests proving accepted/connected sockets are CLOEXEC, control sockets can enable TCP_NODELAY, and TLS accept/read operations respect deadlines.
- [ ] Run `make test-net` and confirm the new assertions fail before implementation.
- [ ] Add CLOEXEC sockets/accept, nonblocking deadline-aware TLS accept/connect helpers, buffered bounded line reads, and TCP_NODELAY helper.
- [ ] Run `make test-net` and full `make test`.
- [ ] Commit `fix: harden network primitives`.

### Task 2: Pairing transcript and session authorization

**Files:** modify `src/session.cpp`, `src/host.cpp`, `include/opal/session.hpp`; test `tests/test_session.cpp` and integration tests.

- [ ] Add tests showing PAIR proof changes with the host certificate fingerprint and a video token cannot be used twice.
- [ ] Verify RED.
- [ ] Bind PAIR HMAC to a versioned transcript containing peer fingerprint, challenge, and client public key. Store structured session records and atomically consume video tokens.
- [ ] Verify focused and full tests.
- [ ] Commit `fix: bind pairing and consume video tokens`.

### Task 3: Host listener/worker hardening

**Files:** modify `src/host.cpp`, `include/opal/host.hpp`; test `tests/test_daemon.cpp`, integration tests.

- [ ] Add tests for daemon loopback binding, bounded pre-auth clients, and auth/video read deadlines.
- [ ] Verify RED.
- [ ] Run daemon listeners on loopback; keep foreground host explicitly reachable. Replace synchronous TLS-in-accept-loop behavior with bounded worker admission and deadline-aware TLS acceptance.
- [ ] Verify focused/full tests.
- [ ] Commit `fix: bound host connection handling`.

### Task 4: Nonblocking low-latency control writer

**Files:** modify `include/opal/session.hpp`, `src/session.cpp`, `src/client.cpp`; test `tests/test_session.cpp`.

- [ ] Add tests for nonblocking enqueue, pointer coalescing, preserved key/button ordering, and control recovery without a writer/heartbeat mutex deadlock.
- [ ] Verify RED.
- [ ] Add a bounded outbound control queue and writer thread; enable TCP_NODELAY; use finite TLS writes; coalesce only pointer-position messages.
- [ ] Verify focused/full tests.
- [ ] Commit `perf: decouple input from control transport`.

### Task 5: Child-process and descriptor lifecycle

**Files:** modify `src/media.cpp`, `src/tunnel.cpp`, `src/host.cpp`, `src/session.cpp`, `src/input_helper.cpp`; test `tests/test_media.cpp`, `tests/test_tunnel.cpp`.

- [ ] Add tests for CLOEXEC descriptors and bounded process-group termination.
- [ ] Verify RED.
- [ ] Use pipe2/socket/open CLOEXEC, direct/process-group spawning where practical, bounded TERM/KILL cleanup, and eliminate session-critical indefinite pclose waits.
- [ ] Verify focused/full tests.
- [ ] Commit `fix: contain child process lifetimes`.

### Task 6: Session-owned zrok access tunnels

**Files:** modify `include/opal/tunnel.hpp`, `src/tunnel.cpp`, `src/session.cpp`; test `tests/test_tunnel.cpp`, `tests/test_tunnel_recovery.cpp`, `tests/test_session.cpp`.

- [ ] Add tests proving two access instances get independent loopback ports and stopping one does not terminate the other.
- [ ] Verify RED.
- [ ] Introduce an owned tunnel-access handle with dynamic ports/PIDs. Remove global client-side `stop_opal_tunnel_processes()` from connect/recovery paths. Reuse a healthy access instance during application reconnect.
- [ ] Verify focused/full tests.
- [ ] Commit `fix: isolate client tunnel sessions`.

### Task 7: Video backpressure and shutdown latency

**Files:** modify `include/opal/host.hpp`, `src/host.cpp`, `src/session.cpp`, `src/media.cpp`; test `tests/test_daemon.cpp`, `tests/test_session.cpp`, `tests/test_media.cpp`.

- [ ] Add tests that a stalled media consumer is dropped on a sub-second deadline and media/player cleanup stays bounded.
- [ ] Verify RED.
- [ ] Reduce stale-media backpressure tolerance, keep recovery generation-based, and make player/capture termination bounded and explicit.
- [ ] Verify focused/full tests.
- [ ] Commit `perf: bound stale video latency`.

### Task 8: Wake bridge, pairing credential lifecycle, and packaging hardening

**Files:** modify `src/wake.cpp`, `src/host.cpp`, `README.md`; tests `tests/test_core.cpp`/new focused tests as appropriate.

- [ ] Add tests for invalid/empty wake secrets, bridge I/O deadlines, and daemon output not exposing the pairing password.
- [ ] Verify RED.
- [ ] Add bounded wake socket I/O, refuse empty secrets, stop daemon password logging, rotate/reopen pairing deliberately, and replace predictable `/tmp/zrok2` installation instructions with `mktemp` + strict shell/checksum-safe flow.
- [ ] Verify focused/full tests.
- [ ] Commit `fix: harden credentials and wake bridge`.

### Task 9: CI stress and sanitizer coverage

**Files:** modify `.github/workflows/ci.yml`, `Makefile`; add stress tests under `tests/`.

- [ ] Add deterministic stress tests for reconnect loops, slow protocol peers, and concurrent session state.
- [ ] Run locally where supported.
- [ ] Add GCC normal, Clang ASan/UBSan, and separate TSan jobs plus stress target.
- [ ] Run the complete workflow and inspect all checks.
- [ ] Commit `test: add hostile transport coverage`.

### Task 10: Final verification

- [ ] Run `make clean && make test`.
- [ ] Run sanitizer/stress targets.
- [ ] Confirm `main` contains only intended changes from the starting commit.
- [ ] Inspect GitHub Actions for the final commit and require green status before declaring completion.
