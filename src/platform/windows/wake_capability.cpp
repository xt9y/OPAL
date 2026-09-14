#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>

#include <opal/wake.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace opal {
namespace {

struct MagicStatus {
    bool known = false;
    bool supported = false;
    bool enabled = false;
};

std::string utf8(const wchar_t* value)
{
    if (!value || !*value) return {};
    const int chars = static_cast<int>(wcslen(value));
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value, chars, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string output(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value, chars, output.data(), bytes, nullptr, nullptr) != bytes) return {};
    return output;
}

std::string trim_copy(std::string value)
{
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) value.pop_back();
    std::size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t' || value[start] == '\r' || value[start] == '\n')) ++start;
    return value.substr(start);
}

std::string ps_quote(const std::string& value)
{
    std::string output = "'";
    for (const char c : value) {
        if (c == '\'') output += "''";
        else output += c;
    }
    output += '\'';
    return output;
}

std::string powershell_capture(const std::string& script)
{
    const std::string command = "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"" + script + "\" 2>NUL";
    FILE* pipe = _popen(command.c_str(), "r");
    if (!pipe) return {};
    std::string output;
    std::array<char, 512> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) output += buffer.data();
    if (_pclose(pipe) != 0) return {};
    return trim_copy(output);
}

bool powershell_run(const std::string& script)
{
    const std::string command = "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"" + script + "\" >NUL 2>NUL";
    return std::system(command.c_str()) == 0;
}

MagicStatus magic_status(const std::string& name)
{
    MagicStatus result;
    const auto value = powershell_capture("$p=Get-NetAdapterPowerManagement -Name " + ps_quote(name) +
        " -ErrorAction Stop; [Console]::Write($p.WakeOnMagicPacket.ToString())");
    if (value.empty()) return result;
    result.known = true;
    if (value == "Unsupported") return result;
    result.supported = true;
    result.enabled = value == "Enabled";
    return result;
}

bool enable_magic(const std::string& name)
{
    return powershell_run("Set-NetAdapterPowerManagement -Name " + ps_quote(name) +
                          " -WakeOnMagicPacket Enabled -ErrorAction Stop");
}

bool fast_startup_enabled(bool& known)
{
    known = false;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power",
                      0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    DWORD type = 0;
    const LSTATUS status = RegQueryValueExW(key, L"HiberbootEnabled", nullptr, &type,
                                            reinterpret_cast<BYTE*>(&value), &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_DWORD) return false;
    known = true;
    return value != 0;
}

bool disable_fast_startup()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power",
                      0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;
    const DWORD value = 0;
    const LSTATUS status = RegSetValueExW(key, L"HiberbootEnabled", 0, REG_DWORD,
                                          reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

std::string format_mac(const BYTE* address, ULONG length)
{
    if (!address || length != 6) return {};
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (ULONG i = 0; i < length; ++i) {
        if (i) output << ':';
        output << std::setw(2) << static_cast<unsigned>(address[i]);
    }
    return output.str();
}

}

WakeCapabilityReport configure_host_wake()
{
    WakeCapabilityReport report;

    bool fast_known = false;
    bool fast_enabled = fast_startup_enabled(fast_known);
    if (fast_known && fast_enabled) {
        if (disable_fast_startup()) fast_enabled = false;
        else report.notes.push_back("Windows Fast Startup is enabled and OPAL could not disable it. Run OPAL once as Administrator for the most reliable shutdown wake behavior.");
    }

    ULONG size = 16384;
    std::vector<unsigned char> storage(size);
    auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    ULONG result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, addresses, &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        storage.resize(size);
        addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, addresses, &size);
    }
    if (result != NO_ERROR) {
        report.notes.push_back("Windows could not enumerate physical network adapters.");
        return report;
    }

    for (auto* current = addresses; current; current = current->Next) {
        WakeLink link = WakeLink::Other;
        if (current->IfType == IF_TYPE_ETHERNET_CSMACD) link = WakeLink::Ethernet;
        else if (current->IfType == IF_TYPE_IEEE80211) link = WakeLink::Wifi;
        else continue;

        const auto mac = format_mac(current->PhysicalAddress, current->PhysicalAddressLength);
        if (mac.empty()) continue;

        WakeAdapterCapability adapter;
        adapter.name = utf8(current->FriendlyName);
        if (adapter.name.empty() && current->AdapterName) adapter.name = current->AdapterName;
        adapter.description = utf8(current->Description);
        adapter.mac = mac;
        adapter.link = link;
        adapter.connected = current->OperStatus == IfOperStatusUp;

        auto magic = magic_status(adapter.name);
        if (magic.known && magic.supported && !magic.enabled) {
            (void)enable_magic(adapter.name);
            magic = magic_status(adapter.name);
        }
        adapter.magic_packet_known = magic.known;
        adapter.magic_packet = magic.supported;
        adapter.configured = magic.enabled;
        adapter.persistent = magic.enabled;

        if (!magic.known) {
            adapter.sleep = adapter.hibernate = adapter.shutdown = WakePowerSupport::Unknown;
            adapter.limitation = "Windows did not expose WakeOnMagicPacket for this adapter. The driver may use a vendor-specific property; update the NIC driver or enable Magic Packet wake in Device Manager.";
        } else if (!magic.supported) {
            adapter.sleep = adapter.hibernate = adapter.shutdown = WakePowerSupport::No;
            adapter.limitation = "The Windows network driver reports Wake on Magic Packet as unsupported.";
        } else if (link == WakeLink::Wifi) {
            adapter.sleep = magic.enabled ? WakePowerSupport::Yes : WakePowerSupport::Unknown;
            adapter.hibernate = WakePowerSupport::Unknown;
            adapter.shutdown = WakePowerSupport::No;
            adapter.limitation = "Wi-Fi uses WoWLAN. Sleep wake can work when the chipset/firmware supports it; OPAL does not promise Wi-Fi wake from shutdown.";
        } else {
            adapter.sleep = magic.enabled ? WakePowerSupport::Yes : WakePowerSupport::Unknown;
            adapter.hibernate = WakePowerSupport::Unknown;
            adapter.shutdown = WakePowerSupport::Unknown;
            adapter.limitation = "Hibernate/shutdown wake also depends on motherboard standby power and firmware PCIe/PME settings. Disable ErP if the NIC loses power while the PC is off.";
            if (fast_known && fast_enabled)
                adapter.limitation += " Windows Fast Startup is still enabled, so shutdown wake may fail.";
        }

        if (magic.supported && !magic.enabled)
            adapter.limitation += " OPAL could not enable the driver setting; run OPAL once as Administrator or enable Wake on Magic Packet in the adapter properties.";
        report.adapters.push_back(std::move(adapter));
    }

    report.preferred = preferred_wake_adapter(report.adapters);
    report.notes.push_back("OPAL never opens a public Wake-on-LAN port. Cross-network wake requires an always-on OPAL/Tailscale relay on the sleeping host's LAN.");
    report.notes.push_back("Tailscale cannot run while the target is off; it carries the authenticated request to the relay, which sends the local Magic Packet.");
    report.notes.push_back("Wake is impossible with the PSU off, AC removed, or firmware configured to remove NIC standby power.");
    return report;
}

}
