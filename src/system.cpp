#include <opal/system.hpp>
#include <opal/config.hpp>
#include <opal/crypto.hpp>
#include <opal/net.hpp>
#include <filesystem>
#include <iostream>
#include <unistd.h>
namespace opal {
int init(){auto p=Paths::load();if(!ensure_layout(p)||!ensure_identity(p.identity_key,p.identity_pub)||!ensure_tls_certificate(p.cert.string(),p.cert_key.string()))return 1;Ini c;if(!std::filesystem::exists(p.config)){c.set("video","fps","60");c.set("video","bitrate_kbps","20000");c.set("video","fullscreen","true");c.set("audio","enabled","true");c.set("network","mode","tunnel");c.set("network","transport","zrok2");c.save(p.config);}std::cout<<"Initialized "<<p.root<<"\n";return 0;}
int doctor(){auto p=Paths::load();std::cout<<"OPAL doctor\n";auto show=[](const char*n,bool ok){std::cout<<(ok?"[ok]   ":"[warn] ")<<n<<"\n";};show("OpenSSL CLI",command_exists("openssl"));show("FFmpeg",command_exists("ffmpeg"));show("FFplay",command_exists("ffplay"));show("GPU Screen Recorder (preferred)",command_exists("gpu-screen-recorder"));show("zrok2 (required transport)",command_exists("zrok2"));show("X11/XWayland client session",std::getenv("DISPLAY")!=nullptr);show("Wayland host session",std::getenv("WAYLAND_DISPLAY")!=nullptr);show("/dev/uinput",access("/dev/uinput",W_OK)==0);show("~/.opal initialized",std::filesystem::exists(p.root));return 0;}
int host_service(bool enable){std::string cmd="systemctl --user ";cmd+=enable?"enable --now opal-host.service":"disable --now opal-host.service";return std::system(cmd.c_str())==0?0:1;}
int bridge_setup(const char*mac){if(!mac||!*mac){std::cerr<<"--mac required\n";return 2;}auto p=Paths::load();ensure_layout(p);Ini c;c.set("bridge","mac",mac);c.set("bridge","secret",random_hex(32));if(!c.save(p.root/"bridge.ini"))return 1;std::cout<<"Bridge configured for "<<mac<<"\nWake secret: "<<c.get("bridge","secret")<<"\nPut this secret in the saved host's wake_secret field.\n";return 0;}
}
