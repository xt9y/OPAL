#pragma once

#include <cstdint>
#include <iostream>
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

inline std::string wake_link_name(WakeLink link)
{
    switch (link) {
    case WakeLink::Ethernet: return "Ethernet";
    case WakeLink::Wifi: return "Wi-Fi";
    default: return "Other";
    }
}

inline std::string wake_power_name(WakePowerSupport support)
{
    switch (support) {
    case WakePowerSupport::Yes: return "yes";
    case WakePowerSupport::No: return "no";
    default: return "unknown";
    }
}

inline bool wake_any_yes(const WakeAdapterCapability& adapter)
{
    return adapter.sleep == WakePowerSupport::Yes ||
           adapter.hibernate == WakePowerSupport::Yes ||
           adapter.shutdown == WakePowerSupport::Yes;
}

inline bool wake_any_unknown(const WakeAdapterCapability& adapter)
{
    return adapter.sleep == WakePowerSupport::Unknown ||
           adapter.hibernate == WakePowerSupport::Unknown ||
           adapter.shutdown == WakePowerSupport::Unknown;
}

inline std::string wake_support_level(const WakeAdapterCapability& adapter)
{
    if (!adapter.magic_packet_known) return "UNVERIFIED";
    if (!adapter.magic_packet) return "UNSUPPORTED";
    if (!adapter.configured) return "NOT CONFIGURED";
    if (!adapter.connected) return "NO LINK";
    if (adapter.sleep == WakePowerSupport::No && adapter.hibernate == WakePowerSupport::No &&
        adapter.shutdown == WakePowerSupport::No) return "UNSUPPORTED";
    if (adapter.persistent && adapter.sleep == WakePowerSupport::Yes &&
        adapter.hibernate == WakePowerSupport::Yes && adapter.shutdown == WakePowerSupport::Yes)
        return "FULL SUPPORT";
    if (wake_any_yes(adapter)) return "PARTIAL SUPPORT";
    return wake_any_unknown(adapter) ? "UNVERIFIED" : "UNSUPPORTED";
}

inline int preferred_wake_adapter(const std::vector<WakeAdapterCapability>& adapters)
{
    int best = -1;
    int best_score = -1;
    for (std::size_t i = 0; i < adapters.size(); ++i) {
        const auto& adapter = adapters[i];
        if (!adapter.magic_packet_known || !adapter.magic_packet || !adapter.configured) continue;
        if (!wake_any_yes(adapter) && !wake_any_unknown(adapter)) continue;
        int score = adapter.link == WakeLink::Ethernet ? 300 : adapter.link == WakeLink::Wifi ? 200 : 100;
        if (adapter.connected) score += 1000;
        if (adapter.shutdown == WakePowerSupport::Yes) score += 15;
        if (adapter.hibernate == WakePowerSupport::Yes) score += 8;
        if (adapter.sleep == WakePowerSupport::Yes) score += 4;
        if (adapter.persistent) score += 3;
        if (score > best_score) { best_score = score; best = static_cast<int>(i); }
    }
    return best;
}

inline void print_wake_report(const WakeCapabilityReport& report)
{
    std::cout << "Wake capability\n--------------------------------\n";
    if (report.adapters.empty()) std::cout << "No physical Ethernet or Wi-Fi adapter was detected.\n";
    for (std::size_t i = 0; i < report.adapters.size(); ++i) {
        const auto& adapter = report.adapters[i];
        std::cout << (static_cast<int>(i) == report.preferred ? "* " : "  ")
                  << (adapter.description.empty() ? adapter.name : adapter.description)
                  << " [" << wake_link_name(adapter.link) << "]\n"
                  << "    interface:    " << adapter.name << '\n'
                  << "    MAC:          " << (adapter.mac.empty() ? "unavailable" : adapter.mac) << '\n'
                  << "    link:         " << (adapter.connected ? "connected" : "disconnected") << '\n'
                  << "    magic packet: " << (!adapter.magic_packet_known ? "unknown" : (adapter.magic_packet ? "supported" : "unsupported")) << '\n'
                  << "    configured:   " << (adapter.configured ? "yes" : "no") << '\n'
                  << "    persistent:   " << (adapter.persistent ? "yes" : "no") << '\n'
                  << "    sleep:        " << wake_power_name(adapter.sleep) << '\n'
                  << "    hibernate:    " << wake_power_name(adapter.hibernate) << '\n'
                  << "    shutdown:     " << wake_power_name(adapter.shutdown) << '\n'
                  << "    status:       " << wake_support_level(adapter) << '\n';
        if (!adapter.connected) std::cout << "    action:       connect this adapter to the target LAN before using remote wake\n";
        if (!adapter.limitation.empty()) std::cout << "    note:         " << adapter.limitation << '\n';
    }
    for (const auto& note : report.notes) std::cout << "[info] " << note << '\n';
    if (report.preferred < 0) std::cout << "Remote wake is unavailable, not configured, or could not be verified.\n";
    else std::cout << "Selected " << report.adapters[static_cast<std::size_t>(report.preferred)].name << " for OPAL remote wake.\n";
}

WakeCapabilityReport configure_host_wake();
#if defined(__linux__)
int configure_linux_wake_admin(const std::string& name, const std::string& mac);
#endif

std::vector<std::uint8_t> wol_packet(const std::string& mac);
bool send_wol(const std::string& mac, const std::string& broadcast = "255.255.255.255", std::uint16_t port = 9);
int run_bridge(std::uint16_t port);
int wake_named(const std::string& name);

}
