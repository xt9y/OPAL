#include <opal/host.hpp>
#include <opal/clipboard.hpp>
#include <opal/config.hpp>
#include <opal/connection_code.hpp>
#include <opal/crypto.hpp>
#include <opal/input.hpp>
#include <opal/media.hpp>
#include <opal/media_profile.hpp>
#include <opal/peer_session.hpp>
#include <opal/pipewire_capture.hpp>
#include <opal/tailnet.hpp>
#include <opal/tailnet_discovery.hpp>
#include <opal/video_feedback.hpp>
#include <opal/video_sender.hpp>

#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <unistd.h>
#include <sys/stat.h>

namespace opal {
namespace {
using Clock=std::chrono::steady_clock;
constexpr std::uint64_t kClipboardReliableWatermark=4;
std::mutex auth_mu,input_mu,test_log_mu;SinkProcess input_sink;Paths G;Ini host_cfg;int host_desktop_width=0,host_desktop_height=0;std::atomic<bool>test_close_used{false};

bool debug_enabled(){const char*v=std::getenv("OPAL_DEBUG");return v&&*v&&std::string(v)!="0";}
bool env_enabled(const char*name){const char*v=std::getenv(name);return v&&*v&&std::string(v)!="0";}
bool read_sdl_clipboard(std::string&text){SDL_ClearError();char*raw=SDL_GetClipboardText();if(!raw)return false;const bool ok=SDL_GetError()[0]=='\0';if(ok)text.assign(raw);SDL_free(raw);return ok;}
bool authorized_unlocked(const std::string&pub){std::ifstream f(G.authorized);std::string line;while(std::getline(f,line)){const auto p=line.find('|');if((p==std::string::npos?line:line.substr(0,p))==pub)return true;}return false;}
bool authorized(const std::string&pub){std::lock_guard<std::mutex>l(auth_mu);return authorized_unlocked(pub);}
std::string sanitize_label(const std::string&label){std::string out;out.reserve(std::min<std::size_t>(128,label.size()));for(unsigned char ch:label){if(out.size()>=128)break;if(ch=='|'||ch=='\r'||ch=='\n'||ch<0x20||ch==0x7f)out+='_';else out+=static_cast<char>(ch);}out=trim(out);return out.empty()?"client":out;}
void authorize(const std::string&pub,const std::string&label){std::lock_guard<std::mutex>l(auth_mu);if(authorized_unlocked(pub))return;std::ofstream f(G.authorized,std::ios::out|std::ios::app);f<<pub<<"|control=1|wake=1|"<<sanitize_label(label)<<"\n";f.close();chmod(G.authorized.c_str(),0600);}
std::string pairing_password(){std::lock_guard<std::mutex>l(auth_mu);Ini cfg;if(!cfg.load(G.host))return{};return normalize_pairing_code(cfg.get("host","password"));}
void rotate_pairing_password(){std::lock_guard<std::mutex>l(auth_mu);Ini cfg;if(!cfg.load(G.host))return;cfg.set("host","password",pairing_code());cfg.save(G.host);host_cfg=cfg;}
std::pair<int,int> desktop_geometry(){const bool was_initialized=(SDL_WasInit(SDL_INIT_VIDEO)&SDL_INIT_VIDEO)!=0;if(!was_initialized&&!SDL_InitSubSystem(SDL_INIT_VIDEO))return{0,0};const SDL_DisplayID display=SDL_GetPrimaryDisplay();const SDL_DisplayMode*mode=display?SDL_GetDesktopDisplayMode(display):nullptr;const std::pair<int,int> result=mode&&mode->w>0&&mode->h>0?std::pair<int,int>{mode->w,mode->h}:std::pair<int,int>{0,0};if(!was_initialized)SDL_QuitSubSystem(SDL_INIT_VIDEO);return result;}
void append_test_log(const char*n,const std::string&line){const char*p=std::getenv(n);if(!p||!*p)return;std::lock_guard<std::mutex>l(test_log_mu);std::ofstream out(p,std::ios::out|std::ios::app);out<<line<<'\n';}
std::string input_helper_command(){const char*e=std::getenv("OPAL_INPUT_HELPER");return e&&*e?e:(access("/usr/local/libexec/opal/opal-input",X_OK)==0?"/usr/local/libexec/opal/opal-input":"./build/opal-input");}
void input_close_locked(){stop_sink(input_sink);}bool input_open_locked(){if(input_sink.pid>0&&input_sink.fd>=0)return true;input_sink=start_sink(input_helper_command());return input_sink.pid>0&&input_sink.fd>=0;}
bool input_write_locked(const std::string&s){if(!input_open_locked())return false;const std::string line=s+"\n";if(!write_sink_timeout(input_sink,line.data(),line.size(),50)){input_close_locked();return false;}return true;}
std::string remap_pointer_line(const std::string&line){if(line.rfind("POINTER ",0)!=0)return line;std::istringstream in(line);std::string word,extra;int x=0,y=0;if(!(in>>word>>x>>y)||in>>extra||word!="POINTER")return line;const auto layout=native_pipewire_layout();if(!layout.valid())return line;const auto mapped=map_composite_pointer(layout,x,y);return "POINTER "+std::to_string(mapped.first)+" "+std::to_string(mapped.second);}
bool input_send(const std::string&s){const auto mapped=remap_pointer_line(s);std::lock_guard<std::mutex>l(input_mu);if(input_write_locked(mapped))return true;return input_write_locked(mapped);}
void track_input(const std::string&line,HeldInputState&held){std::istringstream ss(line);std::string t;ss>>t;if(t=="KEY"){int c=0,d=0;if(ss>>c>>d){if(d)held.press_key(c);else held.release_key(c);}}else if(t=="BUTTON"){int b=0,d=0;if(ss>>b>>d){if(d)held.press_button(b);else held.release_button(b);}}}
bool parse_media_ready(const std::string&line,std::uint32_t&generation,StreamOptions&stream,bool&debug){std::istringstream in(line);std::string word,extra;unsigned long long gen=0;int width=0,height=0,fps=0,dbg=0;if(!(in>>word>>gen>>width>>height>>fps>>dbg)||in>>extra||word!="MEDIA_RECEIVER_READY"||gen==0||gen>0xffffffffULL)return false;const bool native=width==0&&height==0;const bool bounded=width>=16&&height>=16&&width<=16384&&height<=16384;if((!native&&!bounded)||fps<15||fps>240||(dbg!=0&&dbg!=1))return false;generation=static_cast<std::uint32_t>(gen);stream={width,height,fps};debug=dbg==1;return true;}
std::string host_meta(){std::string mac=host_cfg.get("host","mac");if(mac.empty())mac="-";auto tailnet=local_tailnet_ipv4();if(tailnet.empty())tailnet="-";return "HOST_META "+std::to_string(host_desktop_width)+" "+std::to_string(host_desktop_height)+" "+mac+" "+tailnet;}

class HostClipboardBridge {
public:
    explicit HostClipboardBridge(bool allowed):allowed_(allowed){}
    ~HostClipboardBridge(){if(watch_.fd>=0||watch_.pid>0)stop_capture(watch_);}
    void attach(PeerSession*peer){attached_.store(false);{std::lock_guard<std::mutex>lock(receive_mu_);receiver_.reset();pending_remote_.reset();}{std::lock_guard<std::mutex>lock(peer_mu_);peer_=peer;}transport_epoch_.fetch_add(1);attached_.store(true);}
    void detach(PeerSession*peer){attached_.store(false);{std::lock_guard<std::mutex>lock(peer_mu_);if(peer_==peer)peer_=nullptr;}{std::lock_guard<std::mutex>lock(receive_mu_);receiver_.reset();pending_remote_.reset();}}
    void receive_control(const std::string&line){if(!attached_.load())return;std::lock_guard<std::mutex>lock(receive_mu_);std::string completed;const auto status=receiver_.receive(line,completed);if(status==ClipboardReceiveStatus::Complete)pending_remote_=std::move(completed);}
    void pump(){
        if(!ensure_enabled())return;
        const auto epoch=transport_epoch_.load();
        const auto now=Clock::now();
        if(wayland_native_)poll_wayland();else SDL_PumpEvents();
        if(!primed_){std::string local;if(read_local(local)){sender_.prime_local(local);primed_=true;seen_epoch_=epoch;}next_poll_=now+std::chrono::milliseconds(100);}
        else if(epoch!=seen_epoch_){sender_.restart_transport();seen_epoch_=epoch;}
        std::optional<std::string>remote;{std::lock_guard<std::mutex>lock(receive_mu_);if(pending_remote_){remote=std::move(pending_remote_);pending_remote_.reset();}}
        if(remote){if(write_local(*remote)){sender_.note_remote_applied(*remote);primed_=true;}else{if(debug_enabled())std::cerr<<"OPAL clipboard write failed\n";std::lock_guard<std::mutex>lock(receive_mu_);if(!pending_remote_)pending_remote_=std::move(*remote);}}
        if(!wayland_native_&&primed_&&now>=next_poll_){std::string local;if(read_local(local))sender_.observe_local(local);else if(debug_enabled())std::cerr<<"OPAL clipboard read failed: "<<SDL_GetError()<<"\n";next_poll_=now+std::chrono::milliseconds(100);}
        const auto*message=sender_.next_message();if(!message)return;std::lock_guard<std::mutex>lock(peer_mu_);if(peer_&&peer_->running()&&peer_->reliable_pending()<kClipboardReliableWatermark&&peer_->send_input(*message))sender_.pop_message();
    }
private:
    bool wayland_available()const{const char*w=std::getenv("WAYLAND_DISPLAY");return w&&*w&&command_exists("wl-paste")&&command_exists("wl-copy");}
    bool ensure_watch(){
        if(watch_.pid>0&&watch_.fd>=0)return true;
        const auto now=Clock::now();
        if(next_watch_retry_.time_since_epoch().count()!=0&&now<next_watch_retry_)return false;
        watch_=start_capture("wl-paste --type text --watch sh -c 'cat; printf \\\"\\\\0\\\"'");
        if(watch_.pid>0&&watch_.fd>=0)return true;
        next_watch_retry_=now+std::chrono::seconds(1);
        if(debug_enabled())std::cerr<<"OPAL clipboard Wayland watch failed\n";
        return false;
    }
    void poll_wayland(){
        if(!ensure_watch())return;
        char bytes[4096];
        for(int i=0;i<8;++i){
            const int n=read_capture(watch_,bytes,sizeof(bytes),0);if(n==-2)break;if(n<=0){stop_capture(watch_);next_watch_retry_=Clock::now()+std::chrono::seconds(1);break;}
            watch_buffer_.append(bytes,static_cast<std::size_t>(n));
            for(;;){const auto end=watch_buffer_.find('\0');if(end==std::string::npos)break;std::string text=watch_buffer_.substr(0,end);watch_buffer_.erase(0,end+1);if(text.size()<=kClipboardMaxBytes)sender_.observe_local(text);}
            if(watch_buffer_.size()>kClipboardMaxBytes){watch_buffer_.clear();if(debug_enabled())std::cerr<<"OPAL clipboard Wayland event rejected: too large\n";}
        }
    }
    bool read_wayland(std::string&text){
        FILE*f=popen("wl-paste -n --type text 2>/dev/null","r");if(!f)return false;std::string out;char bytes[4096];while(out.size()<=kClipboardMaxBytes){const auto n=fread(bytes,1,sizeof(bytes),f);if(n)out.append(bytes,n);if(n<sizeof(bytes))break;}const int rc=pclose(f);if(rc!=0||out.size()>kClipboardMaxBytes)return false;text=std::move(out);return true;
    }
    bool write_wayland(std::string_view text){
        if(text.empty())return std::system("wl-copy --clear >/dev/null 2>&1")==0;
        FILE*f=popen("wl-copy --type text/plain;charset=utf-8","w");
        if(!f)return false;
        const auto n=fwrite(text.data(),1,text.size(),f);
        const int rc=pclose(f);
        return n==text.size()&&rc==0;
    }
    bool read_local(std::string&text){return wayland_native_?read_wayland(text):read_sdl_clipboard(text);}
    bool write_local(std::string_view text){if(wayland_native_)return write_wayland(text);return SDL_SetClipboardText(std::string(text).c_str());}
    bool ensure_enabled(){
        if(!allowed_)return false;
        if(enabled_)return true;
        const auto now=Clock::now();
        if(next_init_.time_since_epoch().count()!=0&&now<next_init_)return false;
        if(wayland_available()){
            wayland_native_=true;enabled_=true;init_error_logged_=false;ensure_watch();if(debug_enabled())std::cerr<<"OPAL clipboard backend=wayland-native\n";return true;
        }
        const char*w=std::getenv("WAYLAND_DISPLAY");if(w&&*w&&!command_exists("wl-paste")){if(debug_enabled()&&!init_error_logged_)std::cerr<<"OPAL clipboard Wayland backend needs wl-clipboard (wl-paste/wl-copy); falling back to SDL\n";init_error_logged_=true;}
        const bool already=(SDL_WasInit(SDL_INIT_VIDEO)&SDL_INIT_VIDEO)!=0;if(already||SDL_InitSubSystem(SDL_INIT_VIDEO|SDL_INIT_EVENTS)){
            enabled_=true;const char*driver=SDL_GetCurrentVideoDriver();if(debug_enabled())std::cerr<<"OPAL clipboard backend=sdl3 driver="<<(driver&&*driver?driver:"unknown")<<"\n";return true;
        }
        next_init_=now+std::chrono::seconds(1);if(debug_enabled()&&!init_error_logged_)std::cerr<<"OPAL clipboard unavailable: "<<SDL_GetError()<<"\n";init_error_logged_=true;return false;
    }
    bool allowed_=false,enabled_=false,wayland_native_=false,primed_=false,init_error_logged_=false;ClipboardSender sender_;ClipboardReceiver receiver_;std::mutex receive_mu_,peer_mu_;std::optional<std::string>pending_remote_;PeerSession*peer_=nullptr;std::atomic<bool>attached_{false};std::atomic<std::uint64_t>transport_epoch_{0};std::uint64_t seen_epoch_=0;Clock::time_point next_poll_{},next_init_{},next_watch_retry_{};CaptureProcess watch_{};std::string watch_buffer_;
};

bool run_peer_session(TailnetHostResult direct,const std::string&host_public_key,HostClipboardBridge*clipboard){
    const auto client_public_key=direct.client_public_key;const bool was_authorized=authorized(client_public_key);HeldInputState held;VideoSender sender;std::atomic<bool>sender_started{false},sender_starting{false};std::thread sender_start_thread;PeerSession peer;PeerSession*peer_ptr=&peer;VideoSender*sender_ptr=&sender;
    PeerSessionOptions options;options.client_side=false;options.socket=direct.socket;direct.socket={};options.peer=direct.client;options.handshake_timeout_ms=kTailnetPeerHandshakeTimeoutMs;options.handshake.connection_id=direct.connection_id;options.handshake.session_id=direct.session_id;options.handshake.generation=1;options.handshake.client_identity=client_public_key;options.handshake.host_identity=host_public_key;options.handshake.client_nonce=direct.client_nonce;options.handshake.host_nonce=direct.host_nonce;options.handshake.auth_binding=was_authorized?"paired":"pairing";options.identity_private_key=G.identity_key;options.pairing_password=was_authorized?"":pairing_password();
    options.pointer_input=[](const std::string&line){if(line.rfind("POINTER ",0)==0)input_send(line);};
    options.reliable_input=[&](const std::string&line){
        if(line.rfind("KEY ",0)==0||line.rfind("BUTTON ",0)==0){track_input(line,held);input_send(line);return;}
        if(line.rfind("WHEEL ",0)==0||line.rfind("MOUSE ",0)==0){input_send(line);return;}
        if(line.rfind("CLIP ",0)==0){if(clipboard)clipboard->receive_control(line);return;}
        StreamOptions stream;bool debug=false;std::uint32_t media_generation=0;if(parse_media_ready(line,media_generation,stream,debug)){
            if(sender_started.load()||sender_starting.exchange(true))return;
            if(sender_start_thread.joinable())sender_start_thread.join();
            const bool audio=host_cfg.get_bool("audio","enabled",true);const auto media_keys=peer_ptr->media_keys();const auto media_session_id=peer_ptr->session_id();
            sender_start_thread=std::thread([&,media_keys,media_session_id,media_generation,stream,audio,debug]{
                MediaDatagramBatchSend batch=[peer_ptr](std::span<const std::span<const std::uint8_t>>wires){return peer_ptr->send_media_datagrams(wires);};
                if(!sender_ptr->start_native(media_keys,media_session_id,media_generation,stream,audio,[peer_ptr](std::span<const std::uint8_t>wire){return peer_ptr->send_media_datagram(wire);},std::move(batch),[peer_ptr](const std::string&control){return peer_ptr->send_input(control);})){
                    sender_starting.store(false);if(peer_ptr->running())peer_ptr->send_input("MEDIA_ERROR capture-startup");return;
                }
                sender_started.store(true);sender_starting.store(false);if(debug)sender_ptr->handle_control_line(debug_media_request_line(media_generation,true));
            });
            return;
        }
        if(sender_started.load()&&sender_ptr->handle_control_line(line))return;
    };
    std::string error;if(!peer.start(std::move(options),error)){std::cerr<<"OPAL peer session failed: "<<(error.empty()?"unknown error":error)<<"\n";for(const auto&r:held.release_commands())input_send(r);return false;}
    if(clipboard)clipboard->attach(&peer);
    if(!was_authorized){authorize(client_public_key,"client");rotate_pairing_password();append_test_log("OPAL_TEST_AUTH_LOG","PAIR");}else append_test_log("OPAL_TEST_AUTH_LOG","AUTH");
    peer.send_input(host_meta());if(debug_enabled())std::cerr<<"OPAL host peer path="<<peer.path_name()<<" session="<<direct.session_id.substr(0,8)<<"...\n";
    int test_close_ms=0;if(const char*v=std::getenv("OPAL_TEST_CLOSE_FIRST_PEER_MS");v&&*v)try{test_close_ms=std::clamp(std::stoi(v),100,10000);}catch(...){test_close_ms=0;}const bool test_close_this_session=test_close_ms>0&&!test_close_used.exchange(true);const auto test_close_at=Clock::now()+std::chrono::milliseconds(test_close_ms);
    while(peer.running()){if(test_close_this_session&&Clock::now()>=test_close_at){peer.stop();break;}if(clipboard)clipboard->pump();std::this_thread::sleep_for(std::chrono::milliseconds(20));}if(clipboard)clipboard->detach(&peer);if(sender_start_thread.joinable())sender_start_thread.join();sender.stop();for(const auto&r:held.release_commands())input_send(r);peer.stop();return true;
}

int native_host_loop(){
    G=Paths::load();if(!ensure_layout(G))return 1;if(!host_cfg.load(G.host)){if(host_setup()!=0)return 1;if(!host_cfg.load(G.host))return 1;}if(!ensure_identity(G.identity_key,G.identity_pub)){std::cerr<<"identity generation failed\n";return 1;}
    const auto host_public_key=public_key_hex(G.identity_pub);const auto connection_id=connection_id_from_public_key(host_public_key);if(host_public_key.empty()||connection_id.empty()){std::cerr<<"invalid OPAL host identity\n";return 1;}
    HostClipboardBridge clipboard(!env_enabled("OPAL_TEST_HEADLESS"));const auto geo=desktop_geometry();host_desktop_width=geo.first;host_desktop_height=geo.second;
    std::cout<<"OPAL HOST\nConnection code  "<<format_connection_code(connection_id)<<"\nNetwork          Tailscale direct\nWaiting for client...\n"<<std::flush;
    std::string error,bound_tailnet;UdpSocket listener;bool waiting_logged=false;
    for(;;){
        const auto tailnet=local_tailnet_ipv4();
        if(tailnet.empty()){
            if(listener.valid()){close_udp_socket(listener);bound_tailnet.clear();}
            if(debug_enabled()&&!waiting_logged)std::cerr<<"OPAL Tailscale host waiting: Tailscale is not connected\n";
            waiting_logged=true;std::this_thread::sleep_for(std::chrono::seconds(1));continue;
        }
        waiting_logged=false;
        if(!listener.valid()||tailnet!=bound_tailnet){
            if(listener.valid())close_udp_socket(listener);
            listener=open_tailnet_discovery_listener(kTailnetDiscoveryPort,tailnet,error);
            if(!listener.valid()){if(debug_enabled())std::cerr<<"OPAL Tailscale discovery bind failed host="<<tailnet<<" reason="<<error<<"\n";bound_tailnet.clear();std::this_thread::sleep_for(std::chrono::seconds(1));continue;}
            bound_tailnet=tailnet;if(debug_enabled())std::cerr<<"OPAL Tailscale discovery listening "<<tailnet<<":"<<listener.local_port<<"\n";
        }
        TailnetHostResult direct;
        if(!wait_tailnet_client(listener,host_public_key,G.identity_key,direct,500,error)){
            if(error!="Tailscale discovery timeout"){
                if(debug_enabled())std::cerr<<"OPAL Tailscale discovery: "<<error<<"; rebinding\n";
                close_udp_socket(listener);bound_tailnet.clear();std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            continue;
        }
        run_peer_session(std::move(direct),host_public_key,&clipboard);
    }
}

int host_setup(){G=Paths::load();if(!ensure_layout(G))return 1;if(!ensure_identity(G.identity_key,G.identity_pub)){std::cerr<<"identity generation failed\n";return 1;}Ini cfg;if(std::filesystem::exists(G.host))cfg.load(G.host);if(cfg.get("host","password").empty())cfg.set("host","password",pairing_code());cfg.set("video","fps",cfg.get("video","fps","60"));cfg.set("audio","enabled",cfg.get("audio","enabled","true"));if(!cfg.save(G.host))return 1;host_cfg=cfg;const auto pub=public_key_hex(G.identity_pub);const auto id=connection_id_from_public_key(pub);std::cout<<"OPAL host ready.\nConnection code: "<<format_connection_code(id)<<"\nPairing password: "<<cfg.get("host","password")<<"\n";return id.empty()?1:0;}
int host_run(){return native_host_loop();}int host_daemon(){return native_host_loop();}

}
