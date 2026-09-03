#include <opal/system.hpp>
#include <opal/config.hpp>
#include <opal/crypto.hpp>
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <filesystem>
#include <iostream>
#include <unistd.h>
namespace opal {
static bool xinput2_available(){Display*d=XOpenDisplay(nullptr);if(!d)return false;int opcode=0,event=0,error=0;bool ok=XQueryExtension(d,"XInputExtension",&opcode,&event,&error)!=0;if(ok){int major=2,minor=0;ok=XIQueryVersion(d,&major,&minor)==Success&&major>=2;}XCloseDisplay(d);return ok;}
int init(){auto p=Paths::load();if(!ensure_layout(p)||!ensure_identity(p.identity_key,p.identity_pub))return 1;Ini c;if(!std::filesystem::exists(p.config)){c.set("video","fps","60");c.set("video","bitrate_kbps","30000");c.set("video","fullscreen","true");c.set("audio","enabled","true");c.set("network","mode","opal-native");c.set("network","transport","rendezvous+direct-udp+relay");c.save(p.config);}std::cout<<"Initialized "<<p.root<<"\n";return 0;}
int doctor(){auto p=Paths::load();std::cout<<"OPAL doctor\n";auto show=[](const char*n,bool ok){std::cout<<(ok?"[ok]   ":"[warn] ")<<n<<"\n";};show("FFmpeg capture fallback",command_exists("ffmpeg"));show("GPU Screen Recorder (preferred)",command_exists("gpu-screen-recorder"));show("X11/XWayland client display (GLX presenter)",std::getenv("DISPLAY")!=nullptr);show("XInput2 raw client input",xinput2_available());show("PulseAudio/PipeWire audio service",command_exists("pactl")||command_exists("wpctl"));show("Wayland host session",std::getenv("WAYLAND_DISPLAY")!=nullptr);show("/dev/uinput",access("/dev/uinput",W_OK)==0);show("~/.opal initialized",std::filesystem::exists(p.root));std::cout<<"[info] Networking is built into OPAL: signed rendezvous, direct encrypted UDP, blind relay fallback.\n";return 0;}
int host_service(bool enable){std::string cmd="systemctl --user ";cmd+=enable?"enable --now opal-host.service":"disable --now opal-host.service";return std::system(cmd.c_str())==0?0:1;}
int restart_services(){int rc=0;if(std::system("systemctl --user daemon-reload")!=0)rc=1;if(std::system("systemctl --user try-restart opal-host.service")!=0)rc=1;if(std::system("systemctl --user try-restart opal-bridge.service")!=0)rc=1;if(rc==0)std::cout<<"OPAL services restarted.\n";else std::cerr<<"Could not restart all OPAL services.\n";return rc;}
int clean(){auto p=Paths::load();(void)std::system("systemctl --user disable --now opal-host.service >/dev/null 2>&1");(void)std::system("systemctl --user disable --now opal-bridge.service >/dev/null 2>&1");std::error_code ec;std::filesystem::remove_all(p.root,ec);if(ec){std::cerr<<"Could not remove OPAL state: "<<ec.message()<<"\n";return 1;}std::cout<<"OPAL state cleaned.\n";return 0;}
int bridge_setup(const char*mac){if(!mac||!*mac){std::cerr<<"--mac required\n";return 2;}auto p=Paths::load();ensure_layout(p);Ini c;c.set("bridge","mac",mac);c.set("bridge","secret",random_hex(32));if(!c.save(p.root/"bridge.ini"))return 1;std::cout<<"Bridge configured for "<<mac<<"\nWake secret: "<<c.get("bridge","secret")<<"\nPut this secret in the saved host's wake_secret field.\n";return 0;}
}
