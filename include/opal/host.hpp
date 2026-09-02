#pragma once
namespace opal {
inline constexpr int video_backpressure_timeout_ms=5000;
int host_setup();
int host_run();
int host_daemon();
}
