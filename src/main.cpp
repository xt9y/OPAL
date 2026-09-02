#include <opal/client.hpp>
#include <opal/host.hpp>
#include <opal/setup.hpp>
#include <opal/system.hpp>
#include <opal/tunnel.hpp>
#include <opal/wake.hpp>
#include <csignal>
#include <iostream>
#include <string>

static void help() {
    std::cout<<R"(OPAL - performance-first Linux remote desktop

Normal use:
  opal

Advanced commands:
  opal setup
  opal init
  opal clean
  opal host [setup|enable|disable|daemon]
  opal connect <host|ip|zrok:CONTROL,VIDEO> [password]
  opal hosts [list|add <name> <address> [mac]]
  opal wake <saved-host>
  opal bridge [setup --mac MAC|run]
  opal tunnel host
  opal doctor
  opal version
  opal help

Config lives in ~/.opal/ (or OPAL_HOME for testing).
Release remote control with Ctrl+Alt+Shift+Q.
)";
}

int main(int argc,char **argv) {
    signal(SIGPIPE,SIG_IGN);
    if(argc==1) return opal::interactive_run();

    std::string a=argv[1];
    if(a=="help"||a=="--help"||a=="-h") { help(); return 0; }
    if(a=="version"||a=="--version") { std::cout<<"OPAL 0.1.0\n"; return 0; }
    if(a=="setup") return opal::interactive_setup();
    if(a=="init") return opal::init();
    if(a=="clean") return opal::clean();
    if(a=="doctor") return opal::doctor();
    if(a=="host") {
        if(argc>=3&&std::string(argv[2])=="setup") return opal::host_setup();
        if(argc>=3&&std::string(argv[2])=="enable") return opal::host_service(true);
        if(argc>=3&&std::string(argv[2])=="disable") return opal::host_service(false);
        if(argc>=3&&std::string(argv[2])=="daemon") return opal::host_daemon();
        return opal::host_run();
    }
    if(a=="connect"&&argc>=3) return opal::client_connect(argv[2],argc>=4?argv[3]:"");
    if(a=="hosts") {
        if(argc==2||std::string(argv[2])=="list") return opal::hosts_list();
        if(argc>=5&&std::string(argv[2])=="add") return opal::hosts_add(argv[3],argv[4],argc>=6?argv[5]:"");
    }
    if(a=="wake"&&argc>=3) return opal::wake_named(argv[2]);
    if(a=="bridge") {
        if(argc>=3&&std::string(argv[2])=="run") return opal::run_bridge(47992);
        if(argc>=5&&std::string(argv[2])=="setup"&&std::string(argv[3])=="--mac") return opal::bridge_setup(argv[4]);
    }
    if(a=="tunnel"&&argc>=3&&std::string(argv[2])=="host") return opal::tunnel_host();

    std::cerr<<"Unknown command. Run 'opal help'.\n";
    return 2;
}
