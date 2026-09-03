#include <opal/host.hpp>
#include <opal/config.hpp>
#include <opal/crypto.hpp>
#include <opal/direct_video_session.hpp>
#include <opal/input.hpp>
#include <opal/media.hpp>
#include <opal/media_profile.hpp>
#include <opal/net.hpp>
#include <opal/tunnel.hpp>
#include <opal/video_sender.hpp>
#include <X11/Xlib.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>

namespace opal {
static std::mutex auth_mu,input_mu,test_log_mu;
static SinkProcess input_sink;
static Paths G;static Ini host_cfg;static std::atomic<bool> test_control_closed{false};static int host_desktop_width=0,host_desktop_height=0;

static bool authorized(const std::string&pub){std::lock_guard<std::mutex>l(auth_mu);std::ifstream f(G.authorized);std::string line;while(std::getline(f,line)){auto p=line.find('|');if((p==std::string::npos?line:line.substr(0,p))==pub)return true;}return false;}
static std::string sanitize_label(const std::string&label){std::string out;out.reserve(label.size()>128?128:label.size());for(unsigned char ch:label){if(out.size()>=128)break;if(ch=='|'||ch=='\r'||ch=='\n'||ch<0x20||ch==0x7f)out+='_';else out+=static_cast<char>(ch);}out=trim(out);return out.empty()?"client":out;}
static void authorize(const std::string&pub,const std::string&label){if(authorized(pub))return;std::lock_guard<std::mutex>l(auth_mu);std::ofstream f(G.authorized,std::ios::out|std::ios::app);f<<pub<<"|control=1|wake=1|"<<sanitize_label(label)<<"\n";f.close();chmod(G.authorized.c_str(),0600);}
static std::string pairing_password(){std::lock_guard<std::mutex>l(auth_mu);Ini cfg;if(!cfg.load(G.host))return{};return normalize_pairing_code(cfg.get("host","password"));}
static void rotate_pairing_password(){std::lock_guard<std::mutex>l(auth_mu);Ini cfg;if(!cfg.load(G.host))return;cfg.set("host","password",pairing_code());cfg.save(G.host);}
static std::pair<int,int> desktop_geometry(){Display*d=XOpenDisplay(nullptr);if(!d)return{0,0};int s=DefaultScreen(d);int w=DisplayWidth(d,s),h=DisplayHeight(d,s);XCloseDisplay(d);return w>0&&h>0?std::pair<int,int>{w,h}:std::pair<int,int>{0,0};}
static void append_test_log(const char*n,const std::string&line){const char*p=std::getenv(n);if(!p||!*p)return;std::lock_guard<std::mutex>l(test_log_mu);std::ofstream out(p,std::ios::out|std::ios::app);out<<line<<'\n';}
static int test_control_close_after_pings(){const char*v=std::getenv("OPAL_TEST_CONTROL_CLOSE_AFTER_PINGS");if(!v||!*v)return 0;try{int n=std::stoi(v);return n>0?n:0;}catch(...){return 0;}}
static std::string input_helper_command(){const char*e=std::getenv("OPAL_INPUT_HELPER");return e&&*e?e:(access("/usr/local/libexec/opal/opal-input",X_OK)==0?"/usr/local/libexec/opal/opal-input":"./build/opal-input");}
static void input_close_locked(){stop_sink(input_sink);}
static bool input_open_locked(){if(input_sink.pid>0&&input_sink.fd>=0)return true;input_sink=start_sink(input_helper_command());return input_sink.pid>0&&input_sink.fd>=0;}
static bool input_write_locked(const std::string&s){if(!input_open_locked())return false;std::string line=s+"\n";if(!write_sink_timeout(input_sink,line.data(),line.size(),50)){input_close_locked();return false;}return true;}
static bool input_send(const std::string&s){std::lock_guard<std::mutex>l(input_mu);if(input_write_locked(s))return true;return input_write_locked(s);}
static void track_input(const std::string&line,HeldInputState&held){std::istringstream ss(line);std::string t;ss>>t;if(t=="KEY"){int c=0,d=0;if(ss>>c>>d){if(d)held.press_key(c);else held.release_key(c);}}else if(t=="BUTTON"){int b=0,d=0;if(ss>>b>>d){if(d)held.press_button(b);else held.release_button(b);}}}

static bool parse_generation(const std::string &line,std::uint32_t &generation){
    std::istringstream in(line);std::string word,extra;unsigned long long value=0;
    if(!(in>>word>>value)||in>>extra||word!="UDP_GENERATION"||value==0||value>std::numeric_limits<std::uint32_t>::max())return false;
    generation=static_cast<std::uint32_t>(value);return true;
}
static bool parse_profile(const std::string &line,std::uint32_t generation,StreamOptions &stream){
    std::istringstream in(line);std::string word,extra;unsigned long long gen=0;int width=0,height=0,fps=0;
    if(!(in>>word>>gen>>width>>height>>fps)||in>>extra||word!="VIDEO_PROFILE"||gen!=generation)return false;
    const bool native=width==0&&height==0;const bool bounded=width>=16&&height>=16&&width<=16384&&height<=16384;
    if((!native&&!bounded)||fps<15||fps>240)return false;stream.max_width=width;stream.max_height=height;stream.fps=fps;return true;
}
static bool receiver_ready_line(const std::string &line,std::uint32_t generation){
    std::istringstream in(line);std::string word,extra;unsigned long long value=0;
    return (in>>word>>value)&&!(in>>extra)&&word=="DIRECT_RECEIVER_READY"&&value==generation;
}

static void control_client(TlsConn c){
    if(!c.ssl)return;std::mutex send_mu;auto send_line=[&](const std::string &line,int timeout){std::lock_guard<std::mutex>lock(send_mu);return tls_write_line_timeout(c.ssl,line,timeout);};
    std::string challenge=random_hex(32);if(host_desktop_width>0&&host_desktop_height>0){challenge+=" "+std::to_string(host_desktop_width)+" "+std::to_string(host_desktop_height);auto mac=host_cfg.get("host","mac");if(!mac.empty())challenge+=" "+mac;}
    if(!send_line("CHALLENGE OPAL2 "+challenge,2000)){close_tls(c);return;}
    std::string line;if(!tls_read_line_timeout(c.ssl,line,5000)){close_tls(c);return;}std::istringstream ss(line);std::string mode,pub,proof,label;ss>>mode>>pub>>proof;std::getline(ss,label);label=trim(label);bool ok=false;
    if(mode=="AUTH"&&authorized(pub))ok=verify_hex(pub,challenge,proof);else if(mode=="PAIR"){auto password=pairing_password();auto fp=local_fingerprint(c.ssl);auto transcript="OPAL-PAIR-v2\n"+fp+"\n"+challenge+"\n"+pub;ok=!password.empty()&&secure_equal(proof,hmac_sha256_hex(password,transcript));if(ok){authorize(pub,label.empty()?"client":label);rotate_pairing_password();}}
    if(!ok){send_line("DENY",1000);close_tls(c);return;}append_test_log("OPAL_TEST_AUTH_LOG",mode);
    const auto token=random_hex(24);if(!send_line("OK "+token,2000)){close_tls(c);return;}

    std::uint32_t generation=0;StreamOptions stream;
    if(!tls_read_line_timeout(c.ssl,line,2000)||!parse_generation(line,generation)||!tls_read_line_timeout(c.ssl,line,2000)||!parse_profile(line,generation,stream)){send_line("DIRECT_MEDIA_ERROR 0 direct media negotiation protocol error",1000);close_tls(c);return;}
    DirectVideoPath path;std::string negotiation_error;const auto fp=local_fingerprint(c.ssl);
    auto negotiation_send=[&](const std::string &message,int timeout){return send_line(message,timeout);};auto negotiation_read=[&](std::string &message,int timeout){return tls_read_line_timeout(c.ssl,message,timeout);};
    if(!negotiate_host_direct_video(c.ssl,token,pub,fp,generation,default_stun_endpoints(),negotiation_send,negotiation_read,path,negotiation_error,5000)){close_tls(c);return;}

    if(!tls_read_line_timeout(c.ssl,line,3000)||!receiver_ready_line(line,generation)){
        send_line("DIRECT_MEDIA_ERROR "+std::to_string(generation)+" receiver-not-ready",1000);
        close_tls(c);return;
    }

    const bool audio=host_cfg.get_bool("audio","enabled",true);VideoSender sender;
    if(!sender.start(std::move(path),stream,audio,[&](const std::string &message){send_line(message,250);})){send_line("DIRECT_MEDIA_ERROR "+std::to_string(generation)+" capture-startup",1000);close_tls(c);return;}
    if(!send_line("DIRECT_MEDIA_READY "+std::to_string(generation),1000)){sender.stop();close_tls(c);return;}

    HeldInputState held;int ping_count=0;
    while(tls_read_line_timeout(c.ssl,line,15000)){
        if(line=="PING"){
            ++ping_count;int threshold=test_control_close_after_pings();bool expected=false;
            if(threshold>0&&ping_count>=threshold&&test_control_closed.compare_exchange_strong(expected,true)){if(c.fd>=0)shutdown(c.fd,SHUT_RDWR);break;}
            if(!send_line("PONG",1000))break;continue;
        }
        if(line.rfind("KEY ",0)==0||line.rfind("BUTTON ",0)==0){track_input(line,held);input_send(line);continue;}
        if(line.rfind("MOUSE ",0)==0||line.rfind("POINTER ",0)==0||line.rfind("WHEEL ",0)==0){input_send(line);continue;}
        if(sender.handle_control_line(line))continue;
        break;
    }
    sender.stop();for(const auto&r:held.release_commands())input_send(r);close_tls(c);
}

static void control_worker(SSL_CTX*ctx,int lfd){for(;;){auto c=accept_tls_timeout(ctx,lfd,5000);if(c.ssl)control_client(c);}}
static void control_loop(SSL_CTX*ctx,int lfd){std::vector<std::thread>w;for(int i=0;i<8;++i)w.emplace_back(control_worker,ctx,lfd);for(auto&t:w)t.join();}

int host_setup(){
    G=Paths::load();if(!ensure_layout(G))return 1;if(!ensure_identity(G.identity_key,G.identity_pub)){std::cerr<<"identity generation failed\n";return 1;}if(!ensure_tls_certificate(G.cert.string(),G.cert_key.string())){std::cerr<<"TLS certificate generation failed (openssl CLI required)\n";return 1;}
    Ini cfg;if(std::filesystem::exists(G.host))cfg.load(G.host);if(cfg.get("host","password").empty())cfg.set("host","password",pairing_code());cfg.set("host","port","47990");cfg.set("host","video_port","");cfg.set("video","fps",cfg.get("video","fps","60"));cfg.set("video","bitrate_kbps",cfg.get("video","bitrate_kbps","12000"));cfg.set("audio","enabled",cfg.get("audio","enabled","true"));if(!cfg.save(G.host))return 1;
    std::cout<<"OPAL host configured\nPairing password: "<<cfg.get("host","password")<<"\nConfig: "<<G.host<<"\n";return 0;
}
static int host_run_bound(const std::string&bind_address){
    G=Paths::load();ensure_layout(G);if(!host_cfg.load(G.host)){if(host_setup()!=0)return 1;host_cfg.load(G.host);}ensure_identity(G.identity_key,G.identity_pub);ensure_tls_certificate(G.cert.string(),G.cert_key.string());auto geo=desktop_geometry();host_desktop_width=geo.first;host_desktop_height=geo.second;
    SSL_CTX*ctx=server_tls_context(G.cert.string(),G.cert_key.string());if(!ctx){std::cerr<<"cannot create TLS context\n";return 1;}int cp=host_cfg.get_int("host","port",47990);int listener=listen_tcp(static_cast<uint16_t>(cp),bind_address);if(listener<0){SSL_CTX_free(ctx);std::cerr<<"cannot bind OPAL control port\n";return 1;}
    std::cout<<"OPAL HOST\nAddress  "<<(bind_address=="127.0.0.1"?"127.0.0.1":primary_ipv4())<<":"<<cp<<"\nControl  "<<bind_address<<":"<<cp<<"\nVideo    direct encrypted UDP\nWaiting for client...\n";control_loop(ctx,listener);close(listener);SSL_CTX_free(ctx);return 0;
}
int host_run(){return host_run_bound("0.0.0.0");}
int host_daemon(){if(tunnel_host_start()!=0)return 1;std::atomic<bool> supervise{true};std::thread tunnel_thread([&]{tunnel_host_supervise(supervise);});int rc=host_run_bound("127.0.0.1");supervise.store(false);tunnel_thread.join();return rc;}
}
