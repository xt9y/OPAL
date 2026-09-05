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
static bool sdl_video_available(std::string&driver){const bool was_initialized=(SDL_WasInit(SDL_INIT_VIDEO)&SDL_INIT_VIDEO)!=0;if(!was_initialized&&!SDL_InitSubSystem(SDL_INIT_VIDEO))return false;const char*name=SDL_GetCurrentVideoDriver();driver=name&&*name?name:"unknown";if(!was_initialized)SDL_QuitSubSystem(SDL_INIT_VIDEO);return !driver.empty();}
static bool h264_decoder_available(){return avcodec_find_decoder(AV_CODEC_ID_H264)!=nullptr;}
static std::string preferred_h264_encoder(){constexpr const char*names[]={"h264_vaapi","h264_v4l2m2m","h264_nvenc","h264_qsv","libx264","libopenh264"};for(const char*name:names)if(avcodec_find_encoder_by_name(name))return name;return{};}
static std::string preferred_h264_hw_decoder(){const AVCodec*codec=avcodec_find_decoder(AV_CODEC_ID_H264);if(!codec)return{};for(int i=0;;++i){const AVCodecHWConfig*cfg=avcodec_get_hw_config(codec,i);if(!cfg)break;if(!(cfg->methods&AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)||cfg->device_type==AV_HWDEVICE_TYPE_NONE)continue;const char*name=av_hwdevice_get_type_name(cfg->device_type);if(name&&*name)return name;}return{};}
static void import_graphical_environment(){if(!command_exists("systemctl"))return;(void)std::system("systemctl --user import-environment DISPLAY WAYLAND_DISPLAY XDG_RUNTIME_DIR DBUS_SESSION_BUS_ADDRESS XAUTHORITY >/dev/null 2>&1");}
int ensure_tailnet(){
    if(!command_exists("tailscale")){
        if(!command_exists("curl")){std::cerr<<"Tailscale setup needs curl; continuing without WAN tailnet.\n";return 1;}
        std::cout<<"Installing Tailscale...\n"<<std::flush;
        if(std::system("curl -fsSL https://tailscale.com/install.sh | sh")!=0){std::cerr<<"Tailscale installation failed; continuing without WAN tailnet.\n";return 1;}
    }
    if(std::system("tailscale ip -4 >/dev/null 2>&1")==0)return 0;
    if(command_exists("systemctl"))(void)std::system("sudo systemctl enable --now tailscaled >/dev/null 2>&1");
    if(std::system("tailscale ip -4 >/dev/null 2>&1")==0)return 0;
    std::cout<<"Connecting Tailscale...\n"<<std::flush;
    if(std::system("sudo tailscale up")!=0){std::cerr<<"Tailscale login/setup failed; continuing without WAN tailnet.\n";return 1;}
    if(std::system("tailscale ip -4 >/dev/null 2>&1")!=0){std::cerr<<"Tailscale login is not complete; continuing without WAN tailnet.\n";return 1;}
    return 0;
}
int init(){auto p=Paths::load();if(!ensure_layout(p)||!ensure_identity(p.identity_key,p.identity_pub))return 1;Ini c;if(!std::filesystem::exists(p.config)){c.set("video","fps","60");c.set("video","bitrate_kbps","30000");c.set("video","fullscreen","true");c.set("audio","enabled","true");c.set("network","mode","opal-native");c.set("network","transport","rendezvous+direct-udp+relay");c.save(p.config);}std::cout<<"Initialized "<<p.root<<"\n";return 0;}
int doctor(){auto p=Paths::load();std::cout<<"OPAL doctor\n";auto show=[](const char*n,bool ok){std::cout<<(ok?"[ok]   ":"[warn] ")<<n<<"\n";};std::string sdl_driver;const bool sdl_ok=sdl_video_available(sdl_driver);const bool h264_ok=h264_decoder_available();const bool ffmpeg_binary=command_exists("ffmpeg");const bool ffmpeg_stream=ffmpeg_binary&&!capture_command(false,60,4000,false,"",320,180).empty();const auto encoder=preferred_h264_encoder(),hw_decoder=preferred_h264_hw_decoder();show("Native PipeWire/libportal capture build",NativePipeWireVideoCapture::compiled());show((std::string("In-process H.264 encoder (")+(encoder.empty()?"unavailable":encoder)+")").c_str(),!encoder.empty());show((std::string("H.264 hardware decoder candidate (")+(hw_decoder.empty()?"unavailable":hw_decoder)+")").c_str(),!hw_decoder.empty());show("FFmpeg binary",ffmpeg_binary);show("FFmpeg streaming H.264 fallback",ffmpeg_stream);show("Linked FFmpeg H.264 decoder",h264_ok);if(!h264_ok&&std::filesystem::exists("/etc/fedora-release"))std::cout<<"[info] Fedora: enable RPM Fusion Free and install libavcodec-freeworld/ffmpeg-libs for H.264 decode.\n";show("GPU Screen Recorder fallback",command_exists("gpu-screen-recorder"));const std::string sdl_line="SDL3 client video backend ("+(sdl_ok?sdl_driver:std::string("unavailable"))+")";show(sdl_line.c_str(),sdl_ok);show("Wayland clipboard (wl-clipboard)",command_exists("wl-paste")&&command_exists("wl-copy"));show("PulseAudio/PipeWire audio service",command_exists("pactl")||command_exists("wpctl"));show("Tailscale WAN underlay",command_exists("tailscale"));show("Wayland session",std::getenv("WAYLAND_DISPLAY")!=nullptr);show("/dev/uinput",access("/dev/uinput",W_OK)==0);show("~/.opal initialized",std::filesystem::exists(p.root));std::cout<<"[info] Native capture uses PipeWire capture-cycle timestamps; subprocess capture telemetry is explicitly estimated.\n";std::cout<<"[info] Hardware decode prefers DRM PRIME export and the client presenter attempts DMA-BUF/EGL import before CPU upload.\n";std::cout<<"[info] SDL3 client input replaces XInput2 raw client input.\n";std::cout<<"[info] Networking is built into OPAL: LAN first, Tailscale direct WAN, signed rendezvous fallback.\n";return 0;}
int host_service(bool enable){if(enable)import_graphical_environment();std::string cmd="systemctl --user ";cmd+=enable?"enable --now opal-host.service":"disable --now opal-host.service";return std::system(cmd.c_str())==0?0:1;}
int restart_services(){int rc=0;import_graphical_environment();if(std::system("systemctl --user daemon-reload")!=0)rc=1;if(std::system("systemctl --user try-restart opal-host.service")!=0)rc=1;if(std::system("systemctl --user try-restart opal-bridge.service")!=0)rc=1;if(rc==0)std::cout<<"OPAL services restarted.\n";else std::cerr<<"Could not restart all OPAL services.\n";return rc;}
int clean(){auto p=Paths::load();(void)std::system("systemctl --user disable --now opal-host.service >/dev/null 2>&1");(void)std::system("systemctl --user disable --now opal-bridge.service >/dev/null 2>&1");std::error_code ec;std::filesystem::remove_all(p.root,ec);if(ec){std::cerr<<"Could not remove OPAL state: "<<ec.message()<<"\n";return 1;}std::cout<<"OPAL state cleaned.\n";return 0;}
int bridge_setup(const char*mac){if(!mac||!*mac){std::cerr<<"--mac required\n";return 2;}auto p=Paths::load();ensure_layout(p);Ini c;c.set("bridge","mac",mac);c.set("bridge","secret",random_hex(32));if(!c.save(p.root/"bridge.ini"))return 1;std::cout<<"Bridge configured for "<<mac<<"\nWake secret: "<<c.get("bridge","secret")<<"\nPut this secret in the saved host's wake_secret field.\n";return 0;}
}
