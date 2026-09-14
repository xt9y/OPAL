#include <opal/config.hpp>
#include <opal/wake.hpp>

#include <unistd.h>

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

namespace opal {
namespace {

std::string capture_command(const std::string& command)
{
    FILE* pipe = popen((command + " 2>/dev/null").c_str(), "r");
    if (!pipe) return {};
    std::string output;
    std::array<char, 512> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) output += buffer.data();
    if (pclose(pipe) != 0) return {};
    return trim(output);
}

bool contains_case_insensitive(std::string text, std::string needle)
{
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text.find(needle) != std::string::npos;
}

bool pmset_womp_enabled()
{
    const auto output = capture_command("/usr/bin/pmset -g custom");
    return output.find("womp") != std::string::npos && output.find("womp 1") != std::string::npos;
}

bool enable_pmset_womp()
{
    if (pmset_womp_enabled()) return true;
    if (geteuid() == 0) return std::system("/usr/bin/pmset -a womp 1") == 0;
    if (!command_exists("sudo")) return false;
    return std::system("sudo /usr/bin/pmset -a womp 1") == 0;
}

bool interface_active(const std::string& name)
{
    const auto output = capture_command("/sbin/ifconfig " + shell_quote(name));
    return output.find("status: active") != std::string::npos;
}

WakeLink classify_port(const std::string& port)
{
    if (contains_case_insensitive(port, "wi-fi") || contains_case_insensitive(port, "wifi") ||
        contains_case_insensitive(port, "airport"))
        return WakeLink::Wifi;
    if (contains_case_insensitive(port, "ethernet") || contains_case_insensitive(port, "lan"))
        return WakeLink::Ethernet;
    return WakeLink::Other;
}

void add_adapter(WakeCapabilityReport& report, const std::string& port,
                 const std::string& device, const std::string& mac, bool womp)
{
    if (device.empty() || mac.empty()) return;
    WakeAdapterCapability adapter;
    adapter.name = device;
    adapter.description = port.empty() ? device : port;
    adapter.mac = mac;
    adapter.link = classify_port(port);
    adapter.connected = interface_active(device);

    if (adapter.link == WakeLink::Ethernet) {
        adapter.magic_packet_known = true;
        adapter.magic_packet = womp;
        adapter.configured = womp;
        adapter.persistent = womp;
        adapter.sleep = womp ? WakePowerSupport::Yes : WakePowerSupport::Unknown;
        adapter.hibernate = WakePowerSupport::Unknown;
        adapter.shutdown = WakePowerSupport::No;
        adapter.limitation = womp
            ? "macOS network wake is configured for sleep. Traditional PC-style shutdown (S5) Wake-on-LAN is not assumed on Mac hardware; external adapters/docks must retain standby power."
            : "macOS Wake for network access could not be enabled. Administrator permission or hardware support may be required.";
    } else if (adapter.link == WakeLink::Wifi) {
        adapter.magic_packet_known = false;
        adapter.magic_packet = false;
        adapter.configured = womp;
        adapter.persistent = womp;
        adapter.sleep = WakePowerSupport::Unknown;
        adapter.hibernate = WakePowerSupport::Unknown;
        adapter.shutdown = WakePowerSupport::No;
        adapter.limitation = "macOS can use Wake for network access while sleeping, but direct Wi-Fi Magic Packet wake depends on the Mac, access point and sleep mode. Shutdown wake is not supported by OPAL over Wi-Fi.";
    } else {
        adapter.magic_packet_known = false;
        adapter.magic_packet = false;
        adapter.configured = womp;
        adapter.persistent = womp;
        adapter.sleep = WakePowerSupport::Unknown;
        adapter.hibernate = WakePowerSupport::Unknown;
        adapter.shutdown = WakePowerSupport::No;
        adapter.limitation = "This network hardware is not a standard Ethernet/Wi-Fi wake target. OPAL cannot guarantee wake support.";
    }
    report.adapters.push_back(std::move(adapter));
}

}

WakeCapabilityReport configure_host_wake()
{
    WakeCapabilityReport report;
    const bool womp = enable_pmset_womp() && pmset_womp_enabled();
    const auto output = capture_command("/usr/sbin/networksetup -listallhardwareports");

    std::string port;
    std::string device;
    std::string mac;
    std::size_t start = 0;
    while (start <= output.size()) {
        const auto end = output.find('\n', start);
        const auto line = trim(output.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (line.rfind("Hardware Port:", 0) == 0) {
            add_adapter(report, port, device, mac, womp);
            port = trim(line.substr(std::string("Hardware Port:").size()));
            device.clear();
            mac.clear();
        } else if (line.rfind("Device:", 0) == 0) {
            device = trim(line.substr(std::string("Device:").size()));
        } else if (line.rfind("Ethernet Address:", 0) == 0) {
            mac = trim(line.substr(std::string("Ethernet Address:").size()));
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    add_adapter(report, port, device, mac, womp);

    report.preferred = preferred_wake_adapter(report.adapters);
    if (!womp)
        report.notes.push_back("OPAL could not enable macOS 'Wake for network access'. If macOS requested administrator authorization, approve it and run setup again.");
    report.notes.push_back("On Mac hosts, OPAL treats network wake as a sleep feature; it does not promise wake from a fully shut-down Mac.");
    report.notes.push_back("Remote wake from another network still requires an always-on OPAL/Tailscale relay on the Mac's LAN.");
    report.notes.push_back("No software wake works when the Mac has no power or an external Ethernet adapter/dock loses standby power.");
    return report;
}

}
