#include <opal/config.hpp>
#include <opal/wake.hpp>

#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace opal {
namespace {

struct WolInfo {
    bool known = false;
    bool supported = false;
    bool enabled = false;
};

std::string read_text(const std::filesystem::path& path)
{
    std::ifstream input(path);
    std::string value;
    std::getline(input, value);
    return trim(value);
}

bool physical_interface(const std::filesystem::path& entry)
{
    std::error_code error;
    return std::filesystem::exists(entry / "device", error) && !error;
}

bool wifi_interface(const std::filesystem::path& entry)
{
    std::error_code error;
    return std::filesystem::exists(entry / "wireless", error) && !error;
}

WolInfo ethernet_wol(const std::string& name)
{
    WolInfo result;
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return result;

    ifreq request{};
    std::strncpy(request.ifr_name, name.c_str(), IFNAMSIZ - 1);
    ethtool_wolinfo info{};
    info.cmd = ETHTOOL_GWOL;
    request.ifr_data = reinterpret_cast<char*>(&info);
    if (ioctl(fd, SIOCETHTOOL, &request) == 0) {
        result.known = true;
        result.supported = (info.supported & WAKE_MAGIC) != 0;
        result.enabled = (info.wolopts & WAKE_MAGIC) != 0;
    }
    close(fd);
    return result;
}

bool set_ethernet_wol(const std::string& name)
{
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;

    ifreq request{};
    std::strncpy(request.ifr_name, name.c_str(), IFNAMSIZ - 1);
    ethtool_wolinfo info{};
    info.cmd = ETHTOOL_GWOL;
    request.ifr_data = reinterpret_cast<char*>(&info);
    if (ioctl(fd, SIOCETHTOOL, &request) != 0 || !(info.supported & WAKE_MAGIC)) {
        close(fd);
        return false;
    }

    info.cmd = ETHTOOL_SWOL;
    info.wolopts |= WAKE_MAGIC;
    const bool ok = ioctl(fd, SIOCETHTOOL, &request) == 0;
    close(fd);
    return ok;
}

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

std::string active_connection(const std::string& name)
{
    if (!command_exists("nmcli")) return {};
    const auto connection = capture_command("nmcli -g GENERAL.CONNECTION device show " + shell_quote(name));
    return connection == "--" ? std::string{} : connection;
}

bool nm_persistent(const std::string& name, bool wifi)
{
    const auto connection = active_connection(name);
    if (connection.empty()) return false;
    const auto key = wifi ? "802-11-wireless.wake-on-wlan" : "802-3-ethernet.wake-on-lan";
    const auto value = capture_command("nmcli -g " + std::string(key) + " connection show " + shell_quote(connection));
    return value.find("magic") != std::string::npos;
}

bool set_nm_persistent(const std::string& name, bool wifi)
{
    const auto connection = active_connection(name);
    if (connection.empty()) return false;
    const auto key = wifi ? "802-11-wireless.wake-on-wlan" : "802-3-ethernet.wake-on-lan";
    const std::string command = "nmcli connection modify " + shell_quote(connection) + " " + key + " magic";
    if (std::system(command.c_str()) != 0) return false;
    (void)std::system(("nmcli device reapply " + shell_quote(name) + " >/dev/null 2>&1").c_str());
    return true;
}

std::string wifi_phy(const std::filesystem::path& entry)
{
    std::error_code error;
    const auto target = std::filesystem::read_symlink(entry / "phy80211", error);
    return error ? std::string{} : target.filename().string();
}

bool wifi_magic_enabled(const std::filesystem::path& entry, bool& known)
{
    known = false;
    if (!command_exists("iw")) return false;
    const auto phy = wifi_phy(entry);
    if (phy.empty()) return false;
    const auto output = capture_command("iw phy " + shell_quote(phy) + " wowlan show");
    if (output.empty()) return false;
    known = true;
    return output.find("magic packet") != std::string::npos || output.find("magic-packet") != std::string::npos;
}

bool set_wifi_magic(const std::filesystem::path& entry)
{
    if (!command_exists("iw")) return false;
    const auto phy = wifi_phy(entry);
    if (phy.empty()) return false;
    return std::system(("iw phy " + shell_quote(phy) + " wowlan enable magic-packet >/dev/null 2>&1").c_str()) == 0;
}

std::string self_executable()
{
    std::array<char, 4096> buffer{};
    const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    return length > 0 ? std::string(buffer.data(), static_cast<std::size_t>(length)) : std::string{};
}

std::filesystem::path systemd_link_path(const std::string& mac)
{
    std::string id;
    for (const char c : mac) if (c != ':' && c != '-') id += c;
    return std::filesystem::path("/etc/systemd/network") / ("90-opal-wol-" + id + ".link");
}

bool systemd_persistent(const std::string& mac)
{
    if (mac.empty()) return false;
    std::error_code error;
    return std::filesystem::exists(systemd_link_path(mac), error) && !error;
}

bool write_systemd_persistent(const std::string& mac)
{
    if (mac.empty()) return false;
    std::error_code error;
    if (!std::filesystem::exists("/run/systemd/system", error) || error) return false;
    std::filesystem::create_directories("/etc/systemd/network", error);
    if (error) return false;
    std::ofstream output(systemd_link_path(mac), std::ios::out | std::ios::trunc);
    if (!output) return false;
    output << "[Match]\nMACAddress=" << mac << "\n\n[Link]\nWakeOnLan=magic\n";
    return output.good();
}

bool request_admin_config(const std::string& name, const std::string& mac)
{
    const auto executable = self_executable();
    if (executable.empty() || !command_exists("sudo")) return false;
    const std::string command = "sudo " + shell_quote(executable) + " --internal-wake-config " +
                                shell_quote(name) + " " + shell_quote(mac);
    return std::system(command.c_str()) == 0;
}

}

int configure_linux_wake_admin(const std::string& name, const std::string& mac)
{
    if (geteuid() != 0) return 5;
    const std::filesystem::path entry = std::filesystem::path("/sys/class/net") / name;
    if (!physical_interface(entry)) return 4;
    if (read_text(entry / "address") != mac) return 4;

    const bool wifi = wifi_interface(entry);
    const bool configured = wifi ? set_wifi_magic(entry) : set_ethernet_wol(name);
    bool persistent = false;
    if (command_exists("nmcli")) persistent = set_nm_persistent(name, wifi);
    if (!wifi && !persistent) persistent = write_systemd_persistent(mac);
    (void)persistent;
    return configured ? 0 : 3;
}

WakeCapabilityReport configure_host_wake()
{
    WakeCapabilityReport report;
    std::error_code error;
    const std::filesystem::path root("/sys/class/net");
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (error) break;
        const auto name = entry.path().filename().string();
        if (name == "lo" || !physical_interface(entry.path())) continue;

        WakeAdapterCapability adapter;
        adapter.name = name;
        adapter.description = name;
        adapter.mac = read_text(entry.path() / "address");
        const auto state = read_text(entry.path() / "operstate");
        adapter.connected = state == "up" || state == "unknown";
        const bool wifi = wifi_interface(entry.path());
        adapter.link = wifi ? WakeLink::Wifi : WakeLink::Ethernet;

        if (wifi) {
            bool known = false;
            bool enabled = wifi_magic_enabled(entry.path(), known);
            adapter.magic_packet_known = known;
            adapter.magic_packet = known;
            adapter.configured = enabled;
            adapter.persistent = nm_persistent(name, true);

            if (!enabled) {
                bool changed = false;
                if (geteuid() == 0) changed = set_wifi_magic(entry.path());
                else changed = request_admin_config(name, adapter.mac);
                if (changed) enabled = wifi_magic_enabled(entry.path(), known);
                adapter.magic_packet_known = known || changed;
                adapter.magic_packet = known || changed;
                adapter.configured = enabled || changed;
                adapter.persistent = nm_persistent(name, true);
            }

            if (adapter.magic_packet) {
                adapter.sleep = WakePowerSupport::Yes;
                adapter.hibernate = WakePowerSupport::Unknown;
                adapter.shutdown = WakePowerSupport::No;
                adapter.limitation = "Wi-Fi uses WoWLAN. Sleep wake is available; hibernate depends on the chipset/firmware and shutdown wake is not assumed.";
            } else if (adapter.magic_packet_known) {
                adapter.sleep = adapter.hibernate = adapter.shutdown = WakePowerSupport::No;
                adapter.limitation = "This Wi-Fi adapter/driver did not expose Magic Packet WoWLAN. Connect Ethernet for reliable remote wake.";
            } else {
                adapter.sleep = adapter.hibernate = adapter.shutdown = WakePowerSupport::Unknown;
                adapter.limitation = "WoWLAN capability could not be verified. Install the Linux wireless tools (iw) or use Ethernet.";
            }
        } else {
            auto wol = ethernet_wol(name);
            adapter.magic_packet_known = wol.known;
            adapter.magic_packet = wol.supported;
            adapter.configured = wol.enabled;
            adapter.persistent = nm_persistent(name, false) || systemd_persistent(adapter.mac);

            if (wol.supported && (!wol.enabled || !adapter.persistent)) {
                bool changed = false;
                if (geteuid() == 0) changed = configure_linux_wake_admin(name, adapter.mac) == 0;
                else changed = request_admin_config(name, adapter.mac);
                if (changed) {
                    wol = ethernet_wol(name);
                    adapter.magic_packet_known = wol.known;
                    adapter.magic_packet = wol.supported;
                    adapter.configured = wol.enabled;
                    adapter.persistent = nm_persistent(name, false) || systemd_persistent(adapter.mac);
                }
            }

            if (adapter.magic_packet) {
                adapter.sleep = WakePowerSupport::Yes;
                adapter.hibernate = WakePowerSupport::Unknown;
                adapter.shutdown = WakePowerSupport::Unknown;
                adapter.limitation = "Hibernate/shutdown wake also requires NIC standby power and firmware PCIe/PME wake; disable ErP if the NIC loses power when off.";
            } else if (adapter.magic_packet_known) {
                adapter.sleep = adapter.hibernate = adapter.shutdown = WakePowerSupport::No;
                adapter.limitation = "The Ethernet driver reports that Magic Packet wake is unsupported.";
            } else {
                adapter.sleep = adapter.hibernate = adapter.shutdown = WakePowerSupport::Unknown;
                adapter.limitation = "The Ethernet driver's wake capabilities could not be queried.";
            }
        }

        if (adapter.magic_packet && !adapter.configured)
            adapter.limitation += " Administrator permission is required to enable it.";
        if (adapter.configured && !adapter.persistent)
            adapter.limitation += " Wake is enabled now but this Linux networking stack could not be made persistent automatically.";
        report.adapters.push_back(std::move(adapter));
    }

    report.preferred = preferred_wake_adapter(report.adapters);
    report.notes.push_back("OPAL never exposes UDP 7/9 to the Internet. Remote wake outside the host LAN requires an always-on OPAL/Tailscale relay on that LAN.");
    report.notes.push_back("A powered-off NIC cannot run Tailscale itself. Tailscale transports the authenticated request to the relay; the relay emits the local wake packet.");
    report.notes.push_back("No software wake works after AC power is removed, the PSU is switched off, or firmware removes NIC standby power.");
    return report;
}

}
