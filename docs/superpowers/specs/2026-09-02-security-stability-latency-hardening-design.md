# OPAL Security, Stability, and Latency Hardening Design

## Goal

Harden OPAL against hostile or broken peers while reducing latency on the interactive control path without changing the normal `opal` UX or replacing zrok2 as the required network transport.

## Security boundaries

- Normal tunneled host listeners bind loopback only. Direct foreground hosting remains explicitly LAN-reachable.
- Initial pairing cryptographically binds the host TLS certificate fingerprint into the PAIR proof so a first-pair relay cannot poison TOFU state.
- Pairing credentials are not printed by the daemon and can be rotated after successful pairing.
- Video authorization is per authenticated control generation and each issued video token is single-use.
- Pre-auth TLS and protocol reads have hard deadlines and connection concurrency is bounded.
- Child processes inherit no unrelated OPAL file descriptors.
- The wake bridge uses bounded I/O deadlines and refuses empty secrets.

## Control path

The application thread must never block on TLS. `SessionSupervisor` owns a dedicated control writer that accepts small input records through a bounded queue. Key/button/wheel ordering is retained; unsent pointer position records are coalesced so congestion cannot create an input-latency backlog. Heartbeat/recovery no longer owns the writer mutex while waiting for a response.

Control sockets use TCP_NODELAY. Line-oriented authentication remains for compatibility, but steady-state input uses compact binary frames. The host reads buffered frames and passes binary events to the privileged input helper through a Unix-domain socketpair/pipe protocol without per-event stdio flushing/parsing where possible.

## Process and descriptor lifecycle

Sockets, accepted connections, pipes, files opened for children, and uinput handles are close-on-exec. Spawned media processes run in their own process groups and are terminated with bounded TERM/KILL escalation. No `pclose()` or blocking child shutdown is allowed on a session-critical path.

## Tunnel lifecycle

Client zrok access processes are owned by a specific session. Each session uses dynamically allocated loopback ports instead of the fixed 47990/47991 pair. Establishing or recovering one client must not terminate another session or the machine's host-side zrok shares. Host supervision keeps exact-token process validation and adds local endpoint health checks.

## Video behavior

Video remains a separate channel so congestion cannot block input. Host writes have a bounded low-latency deadline; a client that cannot consume current media is disconnected and must reconnect rather than accumulating seconds of obsolete desktop data. Player/capture shutdown is bounded. The existing H.264 low-delay path remains the codec contract for this hardening pass.

## Testing

Add regression coverage for pairing fingerprint binding, single-use video tokens, handshake/read deadlines, loopback daemon binding, descriptor CLOEXEC, TCP_NODELAY, independent tunnel sessions, bounded control queuing/coalescing, wake-bridge timeouts, and process shutdown. CI adds Clang/GCC sanitizer jobs and a stress-oriented test target. All existing tests remain required.
