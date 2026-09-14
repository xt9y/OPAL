#include <opal/wake.hpp>

#include <iostream>

namespace opal {
namespace {

bool any_yes(const WakeAdapterCapability& adapter)
{
    return adapter.sleep == WakePowerSupport::Yes ||
           adapter.hibernate == WakePowerSupport::Yes ||
           adapter.shutdown == WakePowerSupport::Yes;
}

bool any_unknown(const WakeAdapterCapability& adapter)
{
    return adapter.sleep == WakePowerSupport::Unknown ||
           adapter.hibernate == WakePowerSupport::Unknown ||
           adapter.shutdown == WakePowerSupport::Unknown;
}

}

std::string wake_link_name(WakeLink link)
{
    switch (link) {
    case WakeLink::Ethernet: return "Ethernet";
    case WakeLink::Wifi: return "Wi-Fi";
    default: return "Other";
    }
}

std::string wake_power_name(WakePowerSupport support)
{
    switch (support) {
    case WakePowerSupport::Yes: return "yes";
    case WakePowerSupport::No: return "no";
    default: return "unknown";
    }
}

std::string wake_support_level(const WakeAdapterCapability& adapter)
{
    if (!adapter.magic_packet_known) return "UNVERIFIED";
    if (!adapter.magic_packet) return "UNSUPPORTED";
    if (adapter.sleep == WakePowerSupport::No &&
        adapter.hibernate == WakePowerSupport::No &&
        adapter.shutdown == WakePowerSupport::No)
        return "UNSUPPORTED";
    if (adapter.configured && adapter.persistent &&
        adapter.sleep == WakePowerSupport::Yes &&
        adapter.hibernate == WakePowerSupport::Yes &&
        adapter.shutdown == WakePowerSupport::Yes)
        return "FULL SUPPORT";
    if (any_yes(adapter)) return "PARTIAL SUPPORT";
    return any_unknown(adapter) ? "UNVERIFIED" : "UNSUPPORTED";
}

int preferred_wake_adapter(const std::vector<WakeAdapterCapability>& adapters)
{
    int best = -1;
    int best_score = -1;
    for (std::size_t i = 0; i < adapters.size(); ++i) {
        const auto& adapter = adapters[i];
        if ((adapter.magic_packet_known && !adapter.magic_packet) || (!any_yes(adapter) && !any_unknown(adapter))) continue;

        int score = adapter.link == WakeLink::Ethernet ? 300 :
                    adapter.link == WakeLink::Wifi ? 200 : 100;
        if (adapter.connected) score += 20;
        if (adapter.shutdown == WakePowerSupport::Yes) score += 15;
        if (adapter.hibernate == WakePowerSupport::Yes) score += 8;
        if (adapter.sleep == WakePowerSupport::Yes) score += 4;
        if (adapter.persistent) score += 3;
        if (adapter.configured) score += 2;
        if (score > best_score) {
            best_score = score;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void print_wake_report(const WakeCapabilityReport& report)
{
    std::cout << "Wake capability\n--------------------------------\n";
    if (report.adapters.empty()) std::cout << "No physical Ethernet or Wi-Fi adapter was detected.\n";

    for (std::size_t i = 0; i < report.adapters.size(); ++i) {
        const auto& adapter = report.adapters[i];
        std::cout << (static_cast<int>(i) == report.preferred ? "* " : "  ")
                  << (adapter.description.empty() ? adapter.name : adapter.description)
                  << " [" << wake_link_name(adapter.link) << "]\n"
                  << "    interface:   " << adapter.name << '\n'
                  << "    MAC:         " << (adapter.mac.empty() ? "unavailable" : adapter.mac) << '\n'
                  << "    link:        " << (adapter.connected ? "connected" : "disconnected") << '\n'
                  << "    magic packet:" << (!adapter.magic_packet_known ? " unknown" : (adapter.magic_packet ? " supported" : " unsupported")) << '\n'
                  << "    configured:  " << (adapter.configured ? "yes" : "no") << '\n'
                  << "    persistent:  " << (adapter.persistent ? "yes" : "no") << '\n'
                  << "    sleep:       " << wake_power_name(adapter.sleep) << '\n'
                  << "    hibernate:   " << wake_power_name(adapter.hibernate) << '\n'
                  << "    shutdown:    " << wake_power_name(adapter.shutdown) << '\n'
                  << "    status:      " << wake_support_level(adapter) << '\n';
        if (!adapter.limitation.empty()) std::cout << "    note:        " << adapter.limitation << '\n';
    }

    for (const auto& note : report.notes) std::cout << "[info] " << note << '\n';
    if (report.preferred < 0)
        std::cout << "Remote wake is unavailable with the detected hardware/configuration.\n";
    else
        std::cout << "Selected " << report.adapters[static_cast<std::size_t>(report.preferred)].name
                  << " for OPAL remote wake.\n";
}

}
