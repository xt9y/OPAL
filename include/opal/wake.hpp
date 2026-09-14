#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opal {

enum class WakeLink { Ethernet, Wifi, Other };
enum class WakePowerSupport { No, Yes, Unknown };

struct WakeAdapterCapability {
    std::string name;
    std::string description;
    std::string mac;
    WakeLink link = WakeLink::Other;
    bool connected = false;
    bool magic_packet = false;
    bool magic_packet_known = false;
    bool configured = false;
    bool persistent = false;
    WakePowerSupport sleep = WakePowerSupport::Unknown;
    WakePowerSupport hibernate = WakePowerSupport::Unknown;
    WakePowerSupport shutdown = WakePowerSupport::Unknown;
    std::string limitation;
};

struct WakeCapabilityReport {
    std::vector<WakeAdapterCapability> adapters;
    int preferred = -1;
    std::vector<std::string> notes;
};

std::string wake_link_name(WakeLink link);
std::string wake_power_name(WakePowerSupport support);
std::string wake_support_level(const WakeAdapterCapability& adapter);
int preferred_wake_adapter(const std::vector<WakeAdapterCapability>& adapters);
void print_wake_report(const WakeCapabilityReport& report);
WakeCapabilityReport configure_host_wake();

#if defined(__linux__)
int configure_linux_wake_admin(const std::string& name, const std::string& mac);
#endif

std::vector<std::uint8_t> wol_packet(const std::string& mac);
bool send_wol(const std::string& mac, const std::string& broadcast = "255.255.255.255", std::uint16_t port = 9);
int run_bridge(std::uint16_t port);
int wake_named(const std::string& name);

}
