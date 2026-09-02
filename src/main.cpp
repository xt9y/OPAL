#include <opal/host.hpp>
#include <opal/setup.hpp>
#include <opal/system.hpp>
#include <opal/wake.hpp>
#include <csignal>
#include <iostream>
#include <string>

static void help() {
    std::cout<<R"(OPAL - performance-first Linux remote desktop

Commands:
  opal             Wake and connect to the selected host
  opal select      Select a saved host
  opal new         Run OPAL setup / add another host
  opal remove      Remove a saved host
  opal restart     Restart OPAL services
  opal clean       Remove OPAL state and OPAL zrok resources
  opal doctor      Check local OPAL requirements
  opal version     Show the OPAL version
  opal help        Show this help

Config lives in ~/.opal/ (or OPAL_HOME for testing).
Release remote control with Ctrl+Alt+Shift+Q.
)";
}

int main(int argc,char **argv) {
    signal(SIGPIPE,SIG_IGN);
    if(argc==1) return opal::interactive_run();

    std::string a=argv[1];
    if(a=="--internal-host-daemon"&&argc==2) return opal::host_daemon();
    if(a=="--internal-bridge-run"&&argc==2) return opal::run_bridge(47992);

    if(a=="help"||a=="--help"||a=="-h") { help(); return 0; }
    if(a=="version"||a=="--version") { std::cout<<"OPAL 0.1.0\n"; return 0; }
    if(a=="restart"&&argc==2) return opal::restart_services();
    if(a=="clean"&&argc==2) return opal::clean();
    if(a=="select"&&argc==2) return opal::interactive_select();
    if(a=="new"&&argc==2) return opal::interactive_setup();
    if(a=="remove"&&argc==2) return opal::interactive_remove();
    if(a=="doctor"&&argc==2) return opal::doctor();

    std::cerr<<"Unknown command. Run 'opal help'.\n";
    return 2;
}
