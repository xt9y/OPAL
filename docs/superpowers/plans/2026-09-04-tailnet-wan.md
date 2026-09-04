# Zero-Cost Tailnet WAN Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a free WAN path that preserves OPAL's native UDP media by using Tailscale only as an IP underlay.

**Architecture:** The host advertises its Linux `tailscale0` IPv4 address in authenticated host metadata. The client persists that address and, after LAN discovery misses, sends the existing signed discovery exchange directly to that address before trying OPAL rendezvous.

**Tech Stack:** C++20, Linux `getifaddrs`, existing OPAL UDP/crypto/session code, Tailscale underlay.

**Spec:** `docs/superpowers/specs/2026-09-04-tailnet-wan-design.md`

## Global Constraints

- No OPAL-owned VPS or domain dependency.
- No change to OPAL video packetization, FEC, pacing, encryption, decoding, or presentation.
- Tailscale is optional; LAN continues to work without it.
- Saved tailnet addresses are routing hints only; OPAL identity verification remains mandatory.
- No new GitHub Actions workflow runs are required for this change.

---

### Task 1: Detect a valid local Tailscale IPv4 address

**Files:**
- Create: `include/opal/tailnet.hpp`
- Test: `tests/test_session.cpp`

**Interfaces:**
- Produces: `bool is_tailnet_ipv4(const std::string&)`
- Produces: `std::string local_tailnet_ipv4()`

- [x] Add failing boundary tests for the Tailscale CGNAT range `100.64.0.0/10`.
- [x] Verify the test fails before `tailnet.hpp` exists.
- [x] Implement the header-only detector using `inet_pton` and `getifaddrs`, accepting only IPv4 on `tailscale0` inside `100.64.0.0/10`.
- [x] Verify the focused helper test passes.

### Task 2: Advertise and persist the host tailnet address

**Files:**
- Modify: `src/host.cpp`
- Modify: `include/opal/session.hpp`
- Modify: `src/session.cpp`
- Modify: `src/client.cpp`
- Test: `tests/test_session.cpp`

**Interfaces:**
- `HOST_META <width> <height> <mac> <tailnet-or-dash>` remains backward-compatible with the old four-field message.
- `SessionOptions::tailnet_address` carries the saved routing hint.
- `SessionSupervisor::remote_tailnet_address()` exposes authenticated metadata to persistence.

- [x] Extend host metadata with the current `tailscale0` IPv4 or `-`.
- [x] Parse both old and new host metadata formats.
- [x] Load `tailnet_address` from a saved host and save newly learned values back to `hosts.ini`.
- [x] Re-read metadata after media startup to avoid losing a late `HOST_META` delivery.

### Task 3: Prefer signed Tailscale unicast before rendezvous

**Files:**
- Modify: `src/session.cpp`
- Test: `tests/test_session.cpp`

**Interfaces:**
- Reuses `discover_local_host(..., destination_host, kLocalDiscoveryPort)` unchanged.
- Exposes `tailnet-direct` as the client-visible path name.

- [x] Keep 300 ms LAN broadcast first.
- [x] If a valid saved tailnet address and local `tailscale0` exist, try signed unicast discovery for up to 1500 ms.
- [x] Reuse the resulting OPAL UDP socket and normal `PeerSession`; do not add a stream tunnel.
- [x] Fall through to existing rendezvous behavior on failure.
- [x] Add source regression assertions proving LAN precedes tailnet and tailnet precedes rendezvous.

### Task 4: Verification

**Files:**
- Test: `tests/test_session.cpp`

- [x] Verify Tailscale range boundary logic independently with a RED/GREEN compile-run.
- [ ] Run `make test-session` in a full OPAL checkout.
- [ ] Run `make test-local-discovery` in a full OPAL checkout.
- [ ] Build `make -j"$(nproc)"` with the user's installed FFmpeg/GL/Pulse development dependencies.
- [ ] Install on both machines, join the same tailnet, learn the host address over LAN, then verify a hotspot session reports `OPAL network path=tailnet-direct`.
