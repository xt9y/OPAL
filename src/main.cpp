#include <opal/client.hpp>
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
  opal                                      Wake and connect using native resolution at 60 FPS
  opal [--mode max|1080p|1440p|4k] [--fps 15-240]
                                            Connect with temporary stream overrides
  opal select                               Select a saved host
  opal new                                  Run OPAL setup / add another host
  opal remove                               Remove a saved host
  opal restart                              Restart OPAL services
  opal clean                                Remove OPAL state and OPAL zrok resources
  opal doctor                               Check local OPAL requirements
  opal version                              Show the OPAL version
  opal help                                 Show this help

Stream overrides apply only to the current connection. Resolution modes never upscale the host.
Config lives in ~/.opal/ (or OPAL_HOME for testing).
Release remote control with Ctrl+Alt+Shift+Q.
)";
}

static bool parse_fps(const std::string&value,int&fps){
    try{
        size_t used=0;int parsed=std::stoi(value,&used);
        if(used!=value.size()||parsed<15||parsed>240)return false;
        fps=parsed;return true;
    }catch(...){return false;}
}

static int run_stream_flags(int argc,char **argv){
    opal::StreamOptions stream;
    for(int i=1;i<argc;++i){
        std::string flag=argv[i];
        if(flag=="--mode"){
            if(i+1>=argc||!opal::stream_mode_limit(argv[++i],stream.max_width,stream.max_height)){
                std::cerr<<"invalid --mode; expected max, 1080p, 1440p, or 4k\n";return 2;
            }
        }else if(flag=="--fps"){
            if(i+1>=argc||!parse_fps(argv[++i],stream.fps)){
                std::cerr<<"invalid --fps; expected an integer from 15 to 240\n";return 2;
            }
        }else{
            std::cerr<<"Unknown option. Run 'opal help'.\n";return 2;
        }
    }
    return opal::interactive_run(stream);
}

int main(int argc,char **argv) {
    signal(SIGPIPE,SIG_IGN);
    if(argc==1) return opal::interactive_run();

    std::string a=argv[1];
    if(a=="--internal-host-daemon"&&argc==2) return opal::host_daemon();
    if(a=="--internal-bridge-run"&&argc==2) return opal::run_bridge(47992);
    if(a=="--internal-host-setup"&&argc==2) return opal::host_setup();
    if(a=="--internal-host-run"&&argc==2) return opal::host_run();
    if(a=="--internal-connect"&&argc>=3&&argc<=4) return opal::client_connect(argv[2],argc==4?argv[3]:"");

    if(a=="--mode"||a=="--fps") return run_stream_flags(argc,argv);
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
