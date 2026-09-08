#include <opal/config.hpp>
#include <opal/crypto.hpp>
#include <opal/platform.hpp>
#include <opal/system.hpp>

#include <SDL3/SDL.h>
#import <ApplicationServices/ApplicationServices.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <VideoToolbox/VideoToolbox.h>
extern "C" {
#include <libavcodec/avcodec.h>
}

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace opal {
namespace {
bool sdl_video_available(std::string &driver)
{
    const bool initialized = (SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) != 0;
    if (!initialized && !SDL_InitSubSystem(SDL_INIT_VIDEO)) return false;
    const char *name = SDL_GetCurrentVideoDriver();
    driver = name && *name ? name : "unknown";
    if (!initialized) SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return !driver.empty();
}

bool videotoolbox_h264_encoder_available()
{
    CFStringRef encoder_id = nullptr;
    CFDictionaryRef properties = nullptr;
    const OSStatus status = VTCopySupportedPropertyDictionaryForEncoder(
        1280, 720, kCMVideoCodecType_H264, nullptr, &encoder_id, &properties);
    if (encoder_id) CFRelease(encoder_id);
    if (properties) CFRelease(properties);
    return status == noErr;
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
    show("VideoToolbox H.264 encoder", videotoolbox_h264_encoder_available());
    show("VideoToolbox H.264 hardware decoder", VTIsHardwareDecodeSupported(kCMVideoCodecType_H264));
    show("Screen Recording permission", CGPreflightScreenCaptureAccess());
    show("Accessibility input permission", AXIsProcessTrusted());
    show("Tailscale WAN underlay", command_exists("tailscale"));
    show("~/.opal initialized", std::filesystem::exists(paths.root));
    std::cout << "[info] client presenter=sdl3 decoder=libavcodec clipboard=nspasteboard\n";
    std::cout << "[info] host capture=screencapturekit encoder=videotoolbox input=cgevent clipboard=nspasteboard audio=screencapturekit+aac\n";
    return 0;
}

int host_service(bool)
{
    std::cerr << "OPAL macOS host service installation is not enabled yet; run OPAL interactively.\n";
    return 1;
}

int restart_services()
{
    std::cout << "OPAL has no systemd services on macOS.\n";
    return 0;
}

int clean()
{
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
