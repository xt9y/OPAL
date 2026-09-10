#include <opal/system.hpp>
#include <opal/config.hpp>
#include <opal/crypto.hpp>
#include <opal/media.hpp>
#include <opal/pipewire_capture.hpp>

#include <SDL3/SDL.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

namespace opal {
namespace {

bool sdl_video_available(std::string& driver)
{
    const bool initialized = (SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) != 0;
    if (!initialized && !SDL_InitSubSystem(SDL_INIT_VIDEO)) return false;
    const char* name = SDL_GetCurrentVideoDriver();
    driver = name && *name ? name : "unknown";
    if (!initialized) SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return !driver.empty();
}

bool h264_decoder_available()
{
    return avcodec_find_decoder(AV_CODEC_ID_H264) != nullptr;
}

std::string preferred_h264_encoder()
{
    constexpr const char* names[] = {
        "h264_vaapi", "h264_v4l2m2m", "h264_nvenc", "h264_qsv", "libx264", "libopenh264"
    };
    for (const char* name : names) if (avcodec_find_encoder_by_name(name)) return name;
    return {};
}

std::string preferred_h264_hw_decoder()
{
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return {};
    for (int i = 0;; ++i) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
        if (!config) break;
        if (!(config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) ||
            config->device_type == AV_HWDEVICE_TYPE_NONE)
            continue;
        const char* name = av_hwdevice_get_type_name(config->device_type);
        if (name && *name) return name;
    }
    return {};
}

bool environment_value(const char* name)
{
    const char* value = std::getenv(name);
    return value && *value;
}

bool systemd_user_available()
{
    return command_exists("systemctl") &&
           std::system("systemctl --user show-environment >/dev/null 2>&1") == 0;
}

bool user_linger_enabled()
{
    if (!command_exists("loginctl")) return false;
    std::string command = "loginctl show-user " + std::to_string(static_cast<unsigned long>(getuid())) +
                          " -p Linger --value 2>/dev/null | grep -qx yes";
    return std::system(command.c_str()) == 0;
}

void import_graphical_environment()
{
    if (!command_exists("systemctl")) return;
    (void)std::system(
        "systemctl --user import-environment DISPLAY WAYLAND_DISPLAY XDG_RUNTIME_DIR "
        "DBUS_SESSION_BUS_ADDRESS XAUTHORITY >/dev/null 2>&1");
}

void show(const std::string& name, bool ok)
{
    std::cout << (ok ? "[ok]   " : "[warn] ") << name << '\n';
}

}

int ensure_tailnet()
{
    if (!command_exists("tailscale")) {
        if (!command_exists("curl")) {
            std::cerr << "Tailscale setup needs curl; continuing without WAN tailnet.\n";
            return 1;
        }
        std::cout << "Installing Tailscale...\n" << std::flush;
        if (std::system("curl -fsSL https://tailscale.com/install.sh | sh") != 0) {
            std::cerr << "Tailscale installation failed; continuing without WAN tailnet.\n";
            return 1;
        }
    }
    if (std::system("tailscale ip -4 >/dev/null 2>&1") == 0) return 0;
    if (command_exists("systemctl"))
        (void)std::system("sudo systemctl enable --now tailscaled >/dev/null 2>&1");
    if (std::system("tailscale ip -4 >/dev/null 2>&1") == 0) return 0;
    std::cout << "Connecting Tailscale...\n" << std::flush;
    if (std::system("sudo tailscale up") != 0) {
        std::cerr << "Tailscale login/setup failed; continuing without WAN tailnet.\n";
        return 1;
    }
    if (std::system("tailscale ip -4 >/dev/null 2>&1") != 0) {
        std::cerr << "Tailscale login is not complete; continuing without WAN tailnet.\n";
        return 1;
    }
    return 0;
}

int init()
{
    const auto paths = Paths::load();
    if (!ensure_layout(paths) || !ensure_identity(paths.identity_key, paths.identity_pub)) return 1;
    if (!std::filesystem::exists(paths.config)) {
        Ini config;
        config.set("video", "fps", "60");
        config.set("video", "bitrate_kbps", "30000");
        config.set("video", "fullscreen", "true");
        config.set("audio", "enabled", "true");
        config.set("network", "mode", "opal-native");
        config.set("network", "transport", "rendezvous+direct-udp+relay");
        config.save(paths.config);
    }
    std::cout << "Initialized " << paths.root << '\n';
    return 0;
}

int doctor()
{
    const auto paths = Paths::load();
    std::cout << "OPAL doctor\n";
    std::cout << "[info] platform=linux\n";

    std::string sdl_driver;
    const bool sdl_ok = sdl_video_available(sdl_driver);
    const bool native_capture = NativePipeWireVideoCapture::compiled();
    const bool ffmpeg_binary = command_exists("ffmpeg");
    const bool ffmpeg_stream = ffmpeg_binary &&
        !capture_command(false, 60, 4000, false, "", 320, 180).empty();
    const auto encoder = preferred_h264_encoder();
    const auto hw_decoder = preferred_h264_hw_decoder();

    show("Native PipeWire/libportal capture build", native_capture);
    show("In-process H.264 encoder (" + (encoder.empty() ? std::string("unavailable") : encoder) + ")",
         !encoder.empty());
    show("H.264 hardware decoder candidate (" +
         (hw_decoder.empty() ? std::string("unavailable") : hw_decoder) + ")", !hw_decoder.empty());
    show("FFmpeg binary", ffmpeg_binary);
    show("FFmpeg streaming H.264 fallback", ffmpeg_stream);
    show("Linked FFmpeg H.264 decoder", h264_decoder_available());
    show("GPU Screen Recorder fallback", command_exists("gpu-screen-recorder"));
    show("SDL3 client video backend (" + (sdl_ok ? sdl_driver : std::string("unavailable")) + ")", sdl_ok);
    show("Wayland clipboard (wl-clipboard)", command_exists("wl-paste") && command_exists("wl-copy"));
    show("PulseAudio/PipeWire audio service", command_exists("pactl") || command_exists("wpctl"));
    show("Tailscale WAN underlay", command_exists("tailscale"));

    const bool kwin = command_exists("kwin_wayland");
    const bool plasma = command_exists("plasmashell");
    const bool xwayland = command_exists("Xwayland");
    const bool dbus = command_exists("dbus-run-session") ||
                      std::filesystem::exists("/run/user/" + std::to_string(static_cast<unsigned long>(getuid())) + "/bus");
    const bool runtime = environment_value("XDG_RUNTIME_DIR") ||
                         std::filesystem::is_directory("/run/user/" + std::to_string(static_cast<unsigned long>(getuid())));
    const bool graphical = environment_value("WAYLAND_DISPLAY") || environment_value("DISPLAY");
    const bool user_manager = systemd_user_available();
    const bool linger = user_linger_enabled();

    show("KWin Wayland headless compositor", kwin);
    show("Plasma workspace", plasma);
    show("Xwayland compatibility", xwayland);
    show("User D-Bus session", dbus);
    show("XDG runtime directory", runtime);
    show("systemd user manager", user_manager);
    show("systemd user linger", linger);
    show("/dev/uinput", access("/dev/uinput", W_OK) == 0);
    show("~/.opal initialized", std::filesystem::exists(paths.root));

    if (graphical)
        std::cout << "[info] display state=existing-graphical-session\n";
    else if (kwin && plasma && xwayland && dbus && runtime && user_manager)
        std::cout << "[info] display state=headless-ready provider=kwin-virtual+plasma\n";
    else
        std::cout << "[warn] display state=tty-only headless prerequisites incomplete\n";

    if (!linger)
        std::cout << "[info] Enable lingering once so OPAL can start from a TTY-only boot: sudo loginctl enable-linger $USER\n";
    std::cout << "[info] Headless Linux uses KWin virtual Wayland + Plasma + Xwayland and captures the KWin PipeWire node directly.\n";
    std::cout << "[info] Native capture uses PipeWire capture-cycle timestamps; subprocess capture telemetry is explicitly estimated.\n";
    std::cout << "[info] Networking is built into OPAL: LAN first, Tailscale direct WAN, signed rendezvous fallback.\n";
    return 0;
}

int host_service(bool enable)
{
    if (enable) import_graphical_environment();
    std::string command = "systemctl --user ";
    command += enable ? "enable --now opal-host.service" : "disable --now opal-host.service";
    return std::system(command.c_str()) == 0 ? 0 : 1;
}

int restart_services()
{
    int rc = 0;
    import_graphical_environment();
    if (std::system("systemctl --user daemon-reload") != 0) rc = 1;
    if (std::system("systemctl --user try-restart opal-host.service") != 0) rc = 1;
    if (std::system("systemctl --user try-restart opal-bridge.service") != 0) rc = 1;
    if (rc == 0) std::cout << "OPAL services restarted.\n";
    else std::cerr << "Could not restart all OPAL services.\n";
    return rc;
}

int clean()
{
    const auto paths = Paths::load();
    (void)std::system("systemctl --user disable --now opal-host.service >/dev/null 2>&1");
    (void)std::system("systemctl --user disable --now opal-bridge.service >/dev/null 2>&1");
    std::error_code error;
    std::filesystem::remove_all(paths.root, error);
    if (error) {
        std::cerr << "Could not remove OPAL state: " << error.message() << '\n';
        return 1;
    }
    std::cout << "OPAL state cleaned.\n";
    return 0;
}

int bridge_setup(const char* mac)
{
    if (!mac || !*mac) {
        std::cerr << "--mac required\n";
        return 2;
    }
    const auto paths = Paths::load();
    ensure_layout(paths);
    Ini config;
    config.set("bridge", "mac", mac);
    config.set("bridge", "secret", random_hex(32));
    if (!config.save(paths.root / "bridge.ini")) return 1;
    std::cout << "Bridge configured for " << mac << "\nWake secret: " << config.get("bridge", "secret")
              << "\nPut this secret in the saved host's wake_secret field.\n";
    return 0;
}

}
