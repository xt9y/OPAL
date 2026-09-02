#include <opal/client.hpp>
#include <opal/host.hpp>
#include <opal/system.hpp>
#include <opal/tunnel.hpp>
#include <opal/wake.hpp>
#include <opal/config.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <csignal>

static void help(){std::cout<<R"(OPAL - performance-first Linux remote desktop

Usage:
  opal                         interactive menu
  opal init
  opal host [setup|enable|disable]
  opal connect <host|ip|zrok:CONTROL,VIDEO> [password]
  opal hosts [list|add <name> <address> [mac]]
  opal wake <saved-host>
  opal bridge [setup --mac MAC|run]
  opal tunnel host
  opal doctor
  opal version

Config lives in ~/.opal/ (or OPAL_HOME for testing).
Release remote control with Ctrl+Alt+Shift+Q.
)";}
static int menu(){std::cout<<"OPAL\n--------------------------------\n1  Connect to host\n2  Start hosting\n3  Saved hosts\n4  Network/system doctor\n5  Quit\n> ";int n=0;std::cin>>n;if(n==1){std::string h;std::cout<<"Host/IP: ";std::cin>>h;return opal::client_connect(h);}if(n==2)return opal::host_run();if(n==3)return opal::hosts_list();if(n==4)return opal::doctor();return 0;}
int main(int argc,char**argv){signal(SIGPIPE,SIG_IGN);if(argc==1)return menu();std::string a=argv[1];if(a=="help"||a=="--help"||a=="-h"){help();return 0;}if(a=="version"||a=="--version"){std::cout<<"OPAL 0.1.0\n";return 0;}if(a=="init")return opal::init();if(a=="doctor")return opal::doctor();if(a=="host"){if(argc>=3&&std::string(argv[2])=="setup")return opal::host_setup();if(argc>=3&&std::string(argv[2])=="enable")return opal::host_service(true);if(argc>=3&&std::string(argv[2])=="disable")return opal::host_service(false);return opal::host_run();}if(a=="connect"&&argc>=3)return opal::client_connect(argv[2],argc>=4?argv[3]:"");if(a=="hosts"){if(argc==2||std::string(argv[2])=="list")return opal::hosts_list();if(argc>=5&&std::string(argv[2])=="add")return opal::hosts_add(argv[3],argv[4],argc>=6?argv[5]:"");}if(a=="wake"&&argc>=3)return opal::wake_named(argv[2]);if(a=="bridge"){if(argc>=3&&std::string(argv[2])=="run")return opal::run_bridge(47992);if(argc>=5&&std::string(argv[2])=="setup"&&std::string(argv[3])=="--mac")return opal::bridge_setup(argv[4]);}if(a=="tunnel"&&argc>=3&&std::string(argv[2])=="host")return opal::tunnel_host();help();return 2;}
