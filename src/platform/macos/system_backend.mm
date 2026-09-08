#include <opal/config.hpp>
#include <opal/crypto.hpp>
#include <opal/platform.hpp>
#include <opal/system.hpp>

#include <SDL3/SDL.h>
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <VideoToolbox/VideoToolbox.h>
#import <mach-o/dyld.h>
extern "C" {
#include <libavcodec/avcodec.h>
}

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace opal {
namespace {
constexpr const char* kHostLaunchAgentLabel = "de.xt9y.opal.host";

bool sdl_video_available(std::string &driver)
{
    const bool initialized = (SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) != 0;
    if (!initialized && !SDL_InitSubSystem(SDL_INIT_VIDEO)) return false;
    const char *name = SDL_GetCurrentVideoDriver();
    driver = name && *name ? name : "unknown";
    if (!initialized) SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return !driver.empty();
}

bool videotoolbox_h264_hardware_encoder_available()
{
    CFArrayRef encoders = nullptr;
    if (VTCopyVideoEncoderList(nullptr, &encoders) != noErr || !encoders) return false;
    bool available = false;
    const CFIndex count = CFArrayGetCount(encoders);
    for (CFIndex i = 0; i < count && !available; ++i) {
        auto dictionary = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(encoders, i));
        if (!dictionary) continue;
        auto codec = static_cast<CFNumberRef>(CFDictionaryGetValue(dictionary, kVTVideoEncoderList_CodecType));
        auto hardware = static_cast<CFBooleanRef>(CFDictionaryGetValue(dictionary, kVTVideoEncoderList_IsHardwareAccelerated));
        std::int32_t codec_type = 0;
        if (codec && hardware && CFGetTypeID(codec) == CFNumberGetTypeID() &&
            CFGetTypeID(hardware) == CFBooleanGetTypeID() &&
            CFNumberGetValue(codec, kCFNumberSInt32Type, &codec_type) &&
            static_cast<CMVideoCodecType>(codec_type) == kCMVideoCodecType_H264 &&
            CFBooleanGetValue(hardware)) available = true;
    }
    CFRelease(encoders);
    return available;
}

std::filesystem::path user_home()
{
    NSString* home = NSHomeDirectory();
    const char* utf8 = home ? [home UTF8String] : nullptr;
    return utf8 && *utf8 ? std::filesystem::path(utf8) : std::filesystem::path{};
}

std::filesystem::path launch_agent_path()
{
    const auto home = user_home();
    return home.empty() ? std::filesystem::path{} : home / "Library" / "LaunchAgents" / (std::string(kHostLaunchAgentLabel) + ".plist");
}

std::string current_executable_path()
{
    std::array<char, PATH_MAX> fixed{};
    std::uint32_t size = static_cast<std::uint32_t>(fixed.size());
    std::string path;
    if (_NSGetExecutablePath(fixed.data(), &size) == 0) path = fixed.data();
    else {
        std::vector<char> dynamic(static_cast<std::size_t>(size) + 1, '\0');
        if (_NSGetExecutablePath(dynamic.data(), &size) != 0) return {};
        path = dynamic.data();
    }
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    return error ? path : canonical.string();
}

std::string xml_escape(std::string_view value)
{
    std::string out;
    out.reserve(value.size() + 32);
    for (const char c : value) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '\"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string shell_quote(std::string_view value)
{
    std::string out = "'";
    for (const char c : value) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += '\'';
    return out;
}

std::string input_helper_path()
{
    if (const char* configured = std::getenv("OPAL_INPUT_HELPER"); configured && *configured && access(configured, X_OK) == 0)
        return configured;

    const auto executable = current_executable_path();
    if (!executable.empty()) {
        const std::filesystem::path binary(executable);
        const auto adjacent = binary.parent_path() / "opal-input";
        if (access(adjacent.c_str(), X_OK) == 0) return adjacent.string();
        if (binary.parent_path().filename() == "bin") {
            const auto installed = binary.parent_path().parent_path() / "libexec" / "opal" / "opal-input";
            if (access(installed.c_str(), X_OK) == 0) return installed.string();
        }
    }

    for (const char* candidate : {"/usr/local/libexec/opal/opal-input", "/opt/homebrew/libexec/opal/opal-input", "./build/opal-input"})
        if (access(candidate, X_OK) == 0) return candidate;
    return {};
}

bool input_helper_accessible()
{
    const auto helper = input_helper_path();
    if (helper.empty()) return false;
    return std::system((shell_quote(helper) + " --check-access >/dev/null 2>&1").c_str()) == 0;
}

std::string launch_domain()
{
    return "gui/" + std::to_string(static_cast<unsigned long>(getuid()));
}

std::string launch_service_target()
{
    return launch_domain() + "/" + kHostLaunchAgentLabel;
}

bool write_host_launch_agent(const Paths& paths)
{
    const auto plist = launch_agent_path();
    const auto executable = current_executable_path();
    const auto input_helper = input_helper_path();
    if (plist.empty() || executable.empty() || input_helper.empty() || !ensure_layout(paths)) return false;
    std::error_code error;
    std::filesystem::create_directories(plist.parent_path(), error);
    if (error) return false;

    std::ofstream out(plist, std::ios::out | std::ios::trunc);
    if (!out) return false;
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           "<plist version=\"1.0\">\n<dict>\n"
           "  <key>Label</key><string>" << kHostLaunchAgentLabel << "</string>\n"
           "  <key>ProgramArguments</key><array>\n"
           "    <string>" << xml_escape(executable) << "</string>\n"
           "    <string>--internal-host-daemon</string>\n"
           "  </array>\n"
           "  <key>RunAtLoad</key><true/>\n"
           "  <key>KeepAlive</key><true/>\n"
           "  <key>ProcessType</key><string>Interactive</string>\n"
           "  <key>ThrottleInterval</key><integer>2</integer>\n"
           "  <key>EnvironmentVariables</key><dict>\n"
           "    <key>PATH</key><string>/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin</string>\n"
           "    <key>OPAL_INPUT_HELPER</key><string>" << xml_escape(input_helper) << "</string>\n"
           "  </dict>\n"
           "  <key>StandardOutPath</key><string>" << xml_escape((paths.root / "host.log").string()) << "</string>\n"
           "  <key>StandardErrorPath</key><string>" << xml_escape((paths.root / "host.err.log").string()) << "</string>\n"
           "</dict>\n</plist>\n";
    out.close();
    if (!out) return false;
    return chmod(plist.c_str(), 0644) == 0;
}

int launchctl(std::string_view arguments)
{
    const std::string command = "/bin/launchctl " + std::string(arguments);
    return std::system(command.c_str());
}

void bootout_host_agent()
{
    const auto target = launch_service_target();
    (void)launchctl("bootout " + shell_quote(target) + " >/dev/null 2>&1");
}

void write_default_config(const Paths &paths)
{
    if (std::filesystem::exists(paths.config)) return;
    Ini config;
    config.set("video", "fps", "60");
    config.set("video", "bitrate_kbps", "30000");
    config.set("video", "fullscreen", "true");
    config.set("audio", "enabled", "true");
    config.set("network", "mode", "opal-native");
    config.set("network", "transport", "rendezvous+direct-udp+relay");
    (void)config.save(paths.config);
}
}

int ensure_tailnet()
{
    if (command_exists("tailscale")) return 0;
    std::cerr << "Tailscale is not installed; continuing with LAN/rendezvous/relay connectivity.\n";
    return 1;
}

int init()
{
    const auto paths = Paths::load();
    if (!ensure_layout(paths) || !ensure_identity(paths.identity_key, paths.identity_pub)) return 1;
    write_default_config(paths);
    std::cout << "Initialized " << paths.root << "\n";
    return 0;
}

int doctor()
{
    const auto paths = Paths::load();
    std::cout << "OPAL doctor\n";
    std::cout << "[info] platform=" << platform_name(current_platform()) << "\n";
    const auto show=[](const std::string &name,bool ok){std::cout<<(ok?"[ok]   ":"[warn] ")<<name<<"\n";};
    std::string driver;
    const bool sdl_ok = sdl_video_available(driver);
    show("SDL3 client video backend (" + (sdl_ok ? driver : "unavailable") + ")", sdl_ok);
    show("Linked FFmpeg H.264 decoder", avcodec_find_decoder(AV_CODEC_ID_H264) != nullptr);
    show("VideoToolbox H.264 hardware encoder", videotoolbox_h264_hardware_encoder_available());
    show("VideoToolbox H.264 hardware decoder", VTIsHardwareDecodeSupported(kCMVideoCodecType_H264));
    show("Screen Recording permission", CGPreflightScreenCaptureAccess());
    show("Accessibility input helper", input_helper_accessible());
    show("Tailscale WAN underlay", command_exists("tailscale"));
    show("Host LaunchAgent installed", std::filesystem::exists(launch_agent_path()));
    show("~/.opal initialized", std::filesystem::exists(paths.root));
    std::cout << "[info] client presenter=sdl3 decoder=libavcodec clipboard=nspasteboard\n";
    std::cout << "[info] host capture=screencapturekit encoder=videotoolbox-hardware-lowlatency input=cgevent clipboard=nspasteboard audio=screencapturekit+aac\n";
    return 0;
}

int host_service(bool enable)
{
    const auto paths = Paths::load();
    const auto plist = launch_agent_path();
    const auto domain = launch_domain();
    const auto target = launch_service_target();
    if (plist.empty()) {
        std::cerr << "Could not resolve ~/Library/LaunchAgents.\n";
        return 1;
    }

    if (!enable) {
        bootout_host_agent();
        (void)launchctl("disable " + shell_quote(target) + " >/dev/null 2>&1");
        std::error_code error;
        std::filesystem::remove(plist, error);
        return error ? 1 : 0;
    }

    if (!input_helper_accessible()) {
        std::cerr << "OPAL input helper is not Accessibility-authorized. Run 'opal' setup again before enabling the host service.\n";
        return 1;
    }
    if (!write_host_launch_agent(paths)) {
        std::cerr << "Could not write OPAL LaunchAgent at " << plist << ".\n";
        return 1;
    }
    bootout_host_agent();
    (void)launchctl("enable " + shell_quote(target) + " >/dev/null 2>&1");
    if (launchctl("bootstrap " + shell_quote(domain) + " " + shell_quote(plist.string())) != 0) {
        std::cerr << "Could not bootstrap OPAL LaunchAgent.\n";
        return 1;
    }
    if (launchctl("kickstart -k " + shell_quote(target)) != 0) {
        std::cerr << "Could not start OPAL LaunchAgent.\n";
        return 1;
    }
    return 0;
}

int restart_services()
{
    const auto plist = launch_agent_path();
    if (plist.empty() || !std::filesystem::exists(plist)) {
        std::cerr << "OPAL host LaunchAgent is not installed. Run 'opal' and choose host setup first.\n";
        return 1;
    }
    if (launchctl("kickstart -k " + shell_quote(launch_service_target())) != 0) {
        std::cerr << "Could not restart OPAL host LaunchAgent.\n";
        return 1;
    }
    std::cout << "OPAL host service restarted.\n";
    return 0;
}

int clean()
{
    (void)host_service(false);
    const auto paths = Paths::load();
    std::error_code error;
    std::filesystem::remove_all(paths.root, error);
    if (error) {
        std::cerr << "Could not remove OPAL state: " << error.message() << "\n";
        return 1;
    }
    std::cout << "OPAL state cleaned.\n";
    return 0;
}

int bridge_setup(const char *mac)
{
    if (!mac || !*mac) {
        std::cerr << "--mac required\n";
        return 2;
    }
    const auto paths = Paths::load();
    if (!ensure_layout(paths)) return 1;
    Ini config;
    config.set("bridge", "mac", mac);
    config.set("bridge", "secret", random_hex(32));
    if (!config.save(paths.root / "bridge.ini")) return 1;
    std::cout << "Bridge configured for " << mac << "\n";
    return 0;
}

}
