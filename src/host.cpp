#include <opal/host.hpp>
#include <opal/config.hpp>
#include <opal/crypto.hpp>
#include <opal/input.hpp>
#include <opal/media.hpp>
#include <opal/media_profile.hpp>
#include <opal/peer_session.hpp>
#include <opal/rendezvous_client.hpp>
#include <opal/video_feedback.hpp>
#include <opal/video_sender.hpp>

#include <X11/Xlib.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <unistd.h>
#include <sys/stat.h>

namespace opal {
namespace {
using Clock=std::chrono::steady_clock;
std::mutex auth_mu,input_mu,test_log_mu;SinkProcess input_sink;Paths G;Ini host_cfg;int host_desktop_width=0,host_desktop_height=0;std::atomic<bool>test_close_used{false};

bool authorized_unlocked(const std::string&pub){std::ifstream f(G.authorized);std::string line;while(std::getline(f,line)){const auto p=line.find('|');if((p==std::string::npos?line:line.substr(0,p))==pub)return true;}return false;}
bool authorized(const std::string&pub){std::lock_guard<std::mutex>l(auth_mu);return authorized_unlocked(pub);}
std::string sanitize_label(const std::string&label){std::string out;out.reserve(std::min<std::size_t>(128,label.size()));for(unsigned char ch:label){if(out.size()>=128)break;if(ch=='|'||ch=='\r'||ch=='\n'||ch<0x20||ch==0x7f)out+='_';else out+=static_cast<char>(ch);}out=trim(out);return out.empty()?"client":out;}
void authorize(const std::string&pub,const std::string&label){std::lock_guard<std::mutex>l(auth_mu);if(authorized_unlocked(pub))return;std::ofstream f(G.authorized,std::ios::out|std::ios::app);f<<pub<<"|control=1|wake=1|"<<sanitize_label(label)<<"\n";f.close();chmod(G.authorized.c_str(),0600);}
std::string pairing_password(){std::lock_guard<std::mutex>l(auth_mu);Ini cfg;if(!cfg.load(G.host))return{};return normalize_pairing_code(cfg.get("host","password"));}
void rotate_pairing_password(){std::lock_guard<std::mutex>l(auth_mu);Ini cfg;if(!cfg.load(G.host))return;cfg.set("host","password",pairing_code());cfg.save(G.host);host_cfg=cfg;}
std::pair<int,int> desktop_geometry(){Display*d=XOpenDisplay(nullptr);if(!d)return{0,0};const int s=DefaultScreen(d);const int w=DisplayWidth(d,s),h=DisplayHeight(d,s);XCloseDisplay(d);return w>0&&h>0?std::pair<int,int>{w,h}:std::pair<int,int>{0,0};}
void append_test_log(const char*n,const std::string&line){const char*p=std::getenv(n);if(!p||!*p)return;std::lock_guard<std::mutex>l(test_log_mu);std::ofstream out(p,std::ios::out|std::ios::app);out<<line<<'\n';}
std::string input_helper_command(){const char*e=std::getenv("OPAL_INPUT_HELPER");return e&&*e?e:(access("/usr/local/libexec/opal/opal-input",X_OK)==0?"/usr/local/libexec/opal/opal-input":"./build/opal-input");}
void input_close_locked(){stop_sink(input_sink);}bool input_open_locked(){if(input_sink.pid>0&&input_sink.fd>=0)return true;input_sink=start_sink(input_helper_command());return input_sink.pid>0&&input_sink.fd>=0;}
bool input_write_locked(const std::string&s){if(!input_open_locked())return false;const std::string line=s+"\n";if(!write_sink_timeout(input_sink,line.data(),line.size(),50)){input_close_locked();return false;}return true;}
bool input_send(const std::string&s){std::lock_guard<std::mutex>l(input_mu);if(input_write_locked(s))return true;return input_write_locked(s);}
void track_input(const std::string&line,HeldInputState&held){std::istringstream ss(line);std::string t;ss>>t;if(t=="KEY"){int c=0,d=0;if(ss>>c>>d){if(d)held.press_key(c);else held.release_key(c);}}else if(t=="BUTTON"){int b=0,d=0;if(ss>>b>>d){if(d)held.press_button(b);else held.release_button(b);}}}
bool parse_media_ready(const std::string&line,std::uint32_t&generation,StreamOptions&stream,bool&debug){std::istringstream in(line);std::string word,extra;unsigned long long gen=0;int width=0,height=0,fps=0,dbg=0;if(!(in>>word>>gen>>width>>height>>fps>>dbg)||in>>extra||word!="MEDIA_RECEIVER_READY"||gen==0||gen>0xffffffffULL)return false;const bool native=width==0&&height==0;const bool bounded=width>=16&&height>=16&&width<=16384&&height<=16384;if((!native&&!bounded)||fps<15||fps>240||(dbg!=0&&dbg!=1))return false;generation=static_cast<std::uint32_t>(gen);stream={width,height,fps};debug=dbg==1;return true;}
std::string host_meta(){std::string mac=host_cfg.get("host","mac");if(mac.empty())mac="-";return "HOST_META "+std::to_string(host_desktop_width)+" "+std::to_string(host_desktop_height)+" "+mac;}

bool run_native_session(RendezvousClient&rendezvous,const RendezvousMessage&offer,const std::string&host_public_key){
    const bool was_authorized=authorized(offer.public_key);RendezvousIntroduction intro;std::string error;if(!rendezvous.accept_offer(offer,host_public_key,G.identity_key,intro,error)){std::cerr<<"OPAL rendezvous accept failed: "<<error<<"\n";return false;}
    RelayAllocation relay;std::string relay_error;const bool relay_ready=rendezvous.request_relay(intro.session_id,host_public_key,G.identity_key,relay,relay_error);auto socket=rendezvous.take_socket();if(socket.fd<0){std::cerr<<"OPAL rendezvous socket unavailable after introduction\n";return false;}
    HeldInputState held;VideoSender sender;std::atomic<bool>sender_started{false};PeerSession peer;PeerSession*peer_ptr=&peer;VideoSender*sender_ptr=&sender;
    PeerSessionOptions options;options.client_side=false;options.socket=socket;options.peer=intro.peer_observed;if(!intro.peer_local.host.empty()&&intro.peer_local.port)options.lan_peer=intro.peer_local;if(relay_ready)options.relay=PeerRelayFallback{relay.endpoint,relay.allocation_id,RelayRole::Host};options.handshake.rendezvous_id=intro.rendezvous_id;options.handshake.session_id=intro.session_id;options.handshake.generation=1;options.handshake.client_identity=intro.peer_public_key;options.handshake.host_identity=host_public_key;options.handshake.client_nonce=intro.peer_nonce;options.handshake.host_nonce=intro.local_nonce;options.handshake.auth_binding=was_authorized?"paired":"pairing";options.identity_private_key=G.identity_key;options.pairing_password=was_authorized?"":pairing_password();
    options.pointer_input=[](const std::string&line){if(line.rfind("POINTER ",0)==0)input_send(line);};
    options.reliable_input=[&](const std::string&line){
        if(line.rfind("KEY ",0)==0||line.rfind("BUTTON ",0)==0){track_input(line,held);input_send(line);return;}
        if(line.rfind("WHEEL ",0)==0||line.rfind("MOUSE ",0)==0){input_send(line);return;}
        StreamOptions stream;bool debug=false;std::uint32_t media_generation=0;if(parse_media_ready(line,media_generation,stream,debug)){
            if(sender_started.exchange(true))return;
            const bool audio=host_cfg.get_bool("audio","enabled",true);
            if(!sender_ptr->start_native(peer_ptr->media_keys(),peer_ptr->session_id(),media_generation,stream,audio,[peer_ptr](std::span<const std::uint8_t>wire){return peer_ptr->send_media_datagram(wire);},[peer_ptr](const std::string&control){return peer_ptr->send_input(control);})){
                sender_started.store(false);peer_ptr->send_input("MEDIA_ERROR capture-startup");return;
            }
            if(debug)sender_ptr->handle_control_line(debug_media_request_line(media_generation,true));
            return;
        }
        if(sender_started.load()&&sender_ptr->handle_control_line(line))return;
    };
    if(!peer.start(std::move(options),error)){std::cerr<<"OPAL peer session failed: "<<(error.empty()?"unknown error":error)<<"\n";for(const auto&r:held.release_commands())input_send(r);return false;}
    if(!was_authorized){authorize(offer.public_key,"client");rotate_pairing_password();append_test_log("OPAL_TEST_AUTH_LOG","PAIR");}else append_test_log("OPAL_TEST_AUTH_LOG","AUTH");
    peer.send_input(host_meta());if(const char*d=std::getenv("OPAL_DEBUG");d&&*d&&std::string(d)!="0")std::cerr<<"OPAL host peer path="<<peer.path_name()<<" session="<<intro.session_id.substr(0,8)<<"...\n";
    int test_close_ms=0;if(const char*v=std::getenv("OPAL_TEST_CLOSE_FIRST_PEER_MS");v&&*v)try{test_close_ms=std::clamp(std::stoi(v),100,10000);}catch(...){test_close_ms=0;}const bool test_close_this_session=test_close_ms>0&&!test_close_used.exchange(true);const auto test_close_at=Clock::now()+std::chrono::milliseconds(test_close_ms);
    while(peer.running()){if(test_close_this_session&&Clock::now()>=test_close_at){peer.stop();break;}std::this_thread::sleep_for(std::chrono::milliseconds(100));}sender.stop();for(const auto&r:held.release_commands())input_send(r);peer.stop();return true;
}

int native_host_loop(){
    G=Paths::load();if(!ensure_layout(G))return 1;if(!host_cfg.load(G.host)){if(host_setup()!=0)return 1;if(!host_cfg.load(G.host))return 1;}if(!ensure_identity(G.identity_key,G.identity_pub)){std::cerr<<"identity generation failed\n";return 1;}const auto host_public_key=public_key_hex(G.identity_pub);const auto rendezvous_id=rendezvous_id_from_public_key(host_public_key);if(host_public_key.empty()||rendezvous_id.empty()){std::cerr<<"invalid OPAL host identity\n";return 1;}const auto geo=desktop_geometry();host_desktop_width=geo.first;host_desktop_height=geo.second;
    std::cout<<"OPAL HOST\nConnection code  "<<format_connection_code(rendezvous_id)<<"\nVideo            encrypted direct UDP (relay fallback)\nWaiting for client...\n"<<std::flush;
    for(;;){RendezvousClient rendezvous;if(!rendezvous.open()){std::cerr<<"OPAL rendezvous unavailable; retrying...\n";std::this_thread::sleep_for(std::chrono::seconds(2));continue;}std::uint32_t lease_seconds=0;std::string registered,error;if(!rendezvous.register_host(host_public_key,G.identity_key,registered,lease_seconds,error)){std::cerr<<"OPAL rendezvous registration failed: "<<error<<"\n";std::this_thread::sleep_for(std::chrono::seconds(2));continue;}auto renew_at=Clock::now()+std::chrono::seconds(std::max<std::uint32_t>(10,lease_seconds*2/3));bool restart_registration=false;while(!restart_registration){RendezvousMessage offer;std::string wait_error;if(rendezvous.wait_offer(offer,1000,wait_error)){run_native_session(rendezvous,offer,host_public_key);restart_registration=true;break;}if(wait_error!="rendezvous timeout"&&wait_error!="rendezvous protocol timeout"){restart_registration=true;break;}if(Clock::now()>=renew_at){if(!rendezvous.register_host(host_public_key,G.identity_key,registered,lease_seconds,error)){restart_registration=true;break;}renew_at=Clock::now()+std::chrono::seconds(std::max<std::uint32_t>(10,lease_seconds*2/3));}}}
}
}

int host_setup(){G=Paths::load();if(!ensure_layout(G))return 1;if(!ensure_identity(G.identity_key,G.identity_pub)){std::cerr<<"identity generation failed\n";return 1;}Ini cfg;if(std::filesystem::exists(G.host))cfg.load(G.host);if(cfg.get("host","password").empty())cfg.set("host","password",pairing_code());cfg.set("video","fps",cfg.get("video","fps","60"));cfg.set("audio","enabled",cfg.get("audio","enabled","true"));if(!cfg.save(G.host))return 1;host_cfg=cfg;const auto pub=public_key_hex(G.identity_pub);const auto id=rendezvous_id_from_public_key(pub);std::cout<<"OPAL host ready.\nConnection code: "<<format_connection_code(id)<<"\nPairing password: "<<cfg.get("host","password")<<"\n";return id.empty()?1:0;}
int host_run(){return native_host_loop();}int host_daemon(){return native_host_loop();}

}
