#pragma once
namespace opal { int init(); int ensure_tailnet(); int doctor(); int host_service(bool enable); int restart_services(); int clean(); int bridge_setup(const char *mac); }
