#include <opal/host.hpp>
#include <opal/config.hpp>
#include <opal/crypto.hpp>
#include <opal/media.hpp>
#include <opal/net.hpp>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <unistd.h>
#include <sys/stat.h>

namespace opal {
static std::mutex session_mu, auth_mu, input_mu;
static std::unordered_set<std::string> sessions;
static FILE *input_pipe=nullptr;
static Paths G;
static Ini host_cfg;

static bool debug_enabled(){const char*d=std::getenv("OPAL_DEBUG");return d&&*d&&std::string(d)!="0";}
static bool authorized(const std::string&pub){std::lock_guard<std::mutex>l(auth_mu);std::ifstream f(G.authorized);std::string line;while(std::getline(f,line)){auto p=line.find('|');if((p==std::string::npos?line:line.substr(0,p))==pub)return true;}return false;}
static void authorize(const std::string&pub,const std::string&label){if(authorized(pub))return;std::lock_guard<std::mutex>l(auth_mu);std::ofstream f(G.authorized,std::ios::out|std::ios::app);f<<pub<<"|control=1|wake=1|"<<label<<"\n";f.close();chmod(G.authorized.c_str(),0600);}
static void input_send(const std::string&s){std::lock_guard<std::mutex>l(input_mu);if(!input_pipe){const char*e=std::getenv("OPAL_INPUT_HELPER");std::string helper=e&&*e?e:(access("/usr/local/libexec/opal/opal-input",X_OK)==0?"/usr/local/libexec/opal/opal-input":"./build/opal-input");input_pipe=popen(helper.c_str(),"w");}if(input_pipe){std::fputs((s+"\n").c_str(),input_pipe);std::fflush(input_pipe);}}
static void control_client(TlsConn c){if(!c.ssl)return;std::string nonce=random_hex(32);tls_write_line(c.ssl,"CHALLENGE "+nonce);std::string line;if(!tls_read_line(c.ssl,line)){close_tls(c);return;}std::istringstream ss(line);std::string mode,pub,proof,label;ss>>mode>>pub>>proof;std::getline(ss,label);label=trim(label);bool ok=false;if(mode=="AUTH"&&authorized(pub))ok=verify_hex(pub,nonce,proof);else if(mode=="PAIR"){auto password=host_cfg.get("host","password");ok=secure_equal(proof,hmac_sha256_hex(password,nonce+pub));if(ok)authorize(pub,label.empty()?"client":label);}if(!ok){tls_write_line(c.ssl,"DENY");close_tls(c);return;}auto token=random_hex(24);{std::lock_guard<std::mutex>l(session_mu);sessions.insert(token);}tls_write_line(c.ssl,"OK "+token);while(tls_read_line(c.ssl,line)){if(line=="PING"){tls_write_line(c.ssl,"PONG");continue;}if(line.rfind("KEY ",0)==0||line.rfind("MOUSE ",0)==0||line.rfind("BUTTON ",0)==0||line.rfind("WHEEL ",0)==0)input_send(line);}close_tls(c);}
static void control_loop(SSL_CTX*ctx,int lfd){for(;;){auto c=accept_tls(ctx,lfd);if(c.ssl)std::thread(control_client,c).detach();}}
static void video_client(TlsConn c){if(!c.ssl)return;std::string line;if(!tls_read_line(c.ssl,line)){close_tls(c);return;}std::istringstream ss(line);std::string word,token;ss>>word>>token;bool ok=false;{std::lock_guard<std::mutex>l(session_mu);auto it=sessions.find(token);if(word=="VIDEO"&&it!=sessions.end()){sessions.erase(it);ok=true;}}if(!ok){tls_write_line(c.ssl,"DENY");close_tls(c);return;}tls_write_line(c.ssl,"OK");int fps=host_cfg.get_int("video","fps",60),bitrate=host_cfg.get_int("video","bitrate_kbps",20000);bool audio=host_cfg.get_bool("audio","enabled",true);bool gsr=command_exists("gpu-screen-recorder");auto cmd=capture_command(gsr,fps,bitrate,audio);if(const char*e=std::getenv("OPAL_CAPTURE_CMD");e&&*e)cmd=e;if(debug_enabled())std::cerr<<"capture: "<<(std::getenv("OPAL_CAPTURE_CMD")?"test/override":(gsr?"gpu-screen-recorder":"ffmpeg fallback"))<<"\n";FILE*p=popen(cmd.c_str(),"r");if(!p){close_tls(c);return;}char buf[65536];while(size_t n=fread(buf,1,sizeof(buf),p)){if(!tls_write_all(c.ssl,buf,n))break;}pclose(p);close_tls(c);}
static void video_loop(SSL_CTX*ctx,int lfd){for(;;){auto c=accept_tls(ctx,lfd);if(c.ssl)std::thread(video_client,c).detach();}}
int host_setup(){G=Paths::load();if(!ensure_layout(G))return 1;if(!ensure_identity(G.identity_key,G.identity_pub)){std::cerr<<"identity generation failed\n";return 1;}if(!ensure_tls_certificate(G.cert.string(),G.cert_key.string())){std::cerr<<"TLS certificate generation failed (openssl CLI required)\n";return 1;}Ini cfg;if(std::filesystem::exists(G.host))cfg.load(G.host);if(cfg.get("host","password").empty())cfg.set("host","password",pairing_code());cfg.set("host","port","47990");cfg.set("host","video_port","47991");cfg.set("video","fps",cfg.get("video","fps","60"));cfg.set("video","bitrate_kbps",cfg.get("video","bitrate_kbps","20000"));cfg.set("audio","enabled",cfg.get("audio","enabled","true"));if(!cfg.save(G.host))return 1;std::cout<<"OPAL host configured\nPairing password: "<<cfg.get("host","password")<<"\nConfig: "<<G.host<<"\n";return 0;}
int host_run(){G=Paths::load();ensure_layout(G);if(!host_cfg.load(G.host)){if(host_setup()!=0)return 1;host_cfg.load(G.host);}ensure_identity(G.identity_key,G.identity_pub);ensure_tls_certificate(G.cert.string(),G.cert_key.string());SSL_CTX*ctx=server_tls_context(G.cert.string(),G.cert_key.string());if(!ctx){std::cerr<<"cannot create TLS context\n";return 1;}int cp=host_cfg.get_int("host","port",47990),vp=host_cfg.get_int("host","video_port",47991);int lc=listen_tcp(static_cast<uint16_t>(cp)),lv=listen_tcp(static_cast<uint16_t>(vp));if(lc<0||lv<0){std::cerr<<"cannot bind OPAL ports\n";return 1;}std::cout<<"OPAL HOST\nAddress  "<<primary_ipv4()<<":"<<cp<<"\nControl  0.0.0.0:"<<cp<<"\nVideo    0.0.0.0:"<<vp<<"\nPassword "<<host_cfg.get("host","password")<<"\nWaiting for client...\n";std::thread t(control_loop,ctx,lc);video_loop(ctx,lv);t.join();SSL_CTX_free(ctx);return 0;}
}
