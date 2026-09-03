#include <opal/session.hpp>
#include <opal/crypto.hpp>
#include <opal/direct_video_session.hpp>
#include <opal/net.hpp>
#include <opal/tunnel_access.hpp>
#include <opal/video_receiver.hpp>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace opal { namespace {
using namespace std::chrono_literals;

bool wait_for_input(int fd,int timeout_ms){pollfd p{fd,POLLIN,0};for(;;){int rc=poll(&p,1,timeout_ms);if(rc<0&&errno==EINTR)continue;if(rc<=0)return false;if(p.revents&(POLLERR|POLLHUP|POLLNVAL))return false;return(p.revents&POLLIN)!=0;}}
bool pointer_command(const std::string&s){return s.rfind("POINTER ",0)==0;}
bool valid_mac(const std::string&s){if(s.size()!=17)return false;for(size_t i=0;i<s.size();++i){if((i+1)%3==0){if(s[i]!=':')return false;}else if(!std::isxdigit(static_cast<unsigned char>(s[i])))return false;}return true;}
}

struct SessionSupervisor::Impl {
    explicit Impl(SessionOptions o):options(std::move(o)),paired_state(options.paired){}
    SessionOptions options;SSL_CTX*ctx=nullptr;TlsConn control;TunnelAccessHandle tunnel;
    std::mutex control_mu,recovery_mu,queue_mu;mutable std::mutex receiver_mu,state_mu;
    std::condition_variable queue_cv;std::thread control_thread;std::deque<std::string> outbound;std::unique_ptr<VideoReceiver> receiver;
    std::atomic<bool> run{false},paired_state{false};std::atomic<unsigned long> generation{0};std::atomic<int> control_fd{-1};
    std::string observed_fingerprint,remote_mac_value,error;int remote_width_value=0,remote_height_value=0;

    void set_error(const std::string&m){std::lock_guard<std::mutex>l(state_mu);error=m;}
    bool ensure_tunnel(){if(!options.tunneled)return true;if(tunnel_access_healthy(tunnel)){options.control_port=tunnel.control_port;return true;}if(!tunnel_access_start(tunnel,options.control_token,30000)){set_error("OPAL control tunnel access failed");return false;}options.control_port=tunnel.control_port;return true;}
    bool verify_fingerprint(TlsConn&c,std::string&fp){fp=peer_fingerprint(c.ssl);std::string expected;{std::lock_guard<std::mutex>l(state_mu);expected=!options.fingerprint.empty()?options.fingerprint:observed_fingerprint;}if(fp.empty()||(!expected.empty()&&!secure_equal(expected,fp))){set_error(expected.empty()?"host certificate unavailable":"host certificate changed; refusing connection");return false;}return true;}
    bool authenticate(TlsConn&c,bool allow_pair,std::string&token,std::string&fp){
        if(!verify_fingerprint(c,fp))return false;std::string challenge;if(!tls_read_line_timeout(c.ssl,challenge,5000)||challenge.rfind("CHALLENGE ",0)!=0){set_error("control authentication challenge failed");return false;}
        auto payload=challenge.substr(10);if(payload.rfind("OPAL2 ",0)!=0){set_error("incompatible OPAL host protocol; update OPAL on both computers");return false;}auto nonce=payload.substr(6);int pw=0,ph=0;std::string pmac;
        {std::istringstream m(nonce);std::string random,mac,extra;int w=0,h=0;if(m>>random>>w>>h){if(w>0&&h>0){pw=w;ph=h;}if(m>>mac){if(!(m>>extra)&&valid_mac(mac))pmac=mac;else{pw=0;ph=0;pmac.clear();}}}}
        std::string auth;if(paired_state.load()){if(options.client_public_key.empty()||options.client_private_key_path.empty()){set_error("saved client identity unavailable");return false;}auto sig=sign_hex(std::filesystem::path(options.client_private_key_path),nonce);if(sig.empty()){set_error("client authentication signature failed");return false;}auth="AUTH "+options.client_public_key+" "+sig;}else{if(!allow_pair){set_error("paired recovery cannot fall back to pairing");return false;}std::string password=options.pairing_password;if(password.empty()&&options.pairing_password_provider)password=options.pairing_password_provider();password=normalize_pairing_code(password);if(password.empty()){set_error("pairing password required");return false;}auto transcript="OPAL-PAIR-v2\n"+fp+"\n"+nonce+"\n"+options.client_public_key;auth="PAIR "+options.client_public_key+" "+hmac_sha256_hex(password,transcript);if(!options.label.empty())auth+=" "+options.label;}
        if(!tls_write_line_timeout(c.ssl,auth,2000)){set_error("control authentication write failed");return false;}std::string reply;if(!tls_read_line_timeout(c.ssl,reply,5000)){set_error("control authentication reply failed");return false;}if(reply.rfind("OK ",0)!=0){set_error(paired_state.load()?"saved client authentication denied":"pairing password incorrect or expired");return false;}token=reply.substr(3);if(token.empty()){set_error("host returned an empty session token");return false;}
        if((pw>0&&ph>0)||!pmac.empty()){std::lock_guard<std::mutex>l(state_mu);if(pw>0&&ph>0){remote_width_value=pw;remote_height_value=ph;}if(!pmac.empty())remote_mac_value=pmac;}paired_state.store(true);return true;
    }
    bool connect_authenticated(bool allow_pair,TlsConn&out,std::string&token,std::string&fp){auto c=connect_tls_retry(ctx,options.target,static_cast<uint16_t>(options.control_port),10000,100);if(!c.ssl){set_error("cannot connect to OPAL host");return false;}if(!authenticate(c,allow_pair,token,fp)){close_tls(c);return false;}out=c;c={};return true;}
    bool negotiate_media(TlsConn &c,const std::string &token,const std::string &fp,unsigned long g,DirectVideoPath &path){
        if(g==0||g>0xffffffffUL){set_error("invalid control generation");return false;}const auto gs=std::to_string(g);
        if(!tls_write_line_timeout(c.ssl,"UDP_GENERATION "+gs,1000)||!tls_write_line_timeout(c.ssl,"VIDEO_PROFILE "+gs+" "+std::to_string(options.stream.max_width)+" "+std::to_string(options.stream.max_height)+" "+std::to_string(options.stream.fps),1000)){set_error("direct UDP profile exchange failed");return false;}
        std::string negotiation_error;auto send=[&](const std::string &line,int timeout){return tls_write_line_timeout(c.ssl,line,timeout);};auto read=[&](std::string &line,int timeout){return tls_read_line_timeout(c.ssl,line,timeout);};
        if(!negotiate_client_direct_video(c.ssl,token,options.client_public_key,fp,static_cast<std::uint32_t>(g),default_stun_endpoints(),send,read,path,negotiation_error,5000)){set_error(negotiation_error.empty()?direct_video_unavailable_error():negotiation_error);return false;}
        std::string result;if(!tls_read_line_timeout(c.ssl,result,12000)){set_error("direct media startup timed out");return false;}if(result=="DIRECT_MEDIA_READY "+gs)return true;
        const std::string prefix="DIRECT_MEDIA_ERROR "+gs+" ";if(result.rfind(prefix,0)==0){set_error(result.substr(prefix.size()));return false;}set_error("direct media startup protocol error");return false;
    }
    void install_control(TlsConn&next,const std::string&fp,unsigned long g){{std::lock_guard<std::mutex>l(control_mu);close_tls(control);control=next;next={};control_fd.store(control.fd);} {std::lock_guard<std::mutex>l(state_mu);if(observed_fingerprint.empty())observed_fingerprint=fp;if(options.fingerprint.empty())options.fingerprint=observed_fingerprint;error.clear();}generation.store(g);}
    void stop_receiver(){std::unique_ptr<VideoReceiver> old;{std::lock_guard<std::mutex>l(receiver_mu);old=std::move(receiver);}if(old)old->stop();}
    bool start_receiver(DirectVideoPath path){auto next=std::make_unique<VideoReceiver>();if(!next->start(std::move(path),[this](const std::string &line){this->enqueue(line);})){set_error("could not start direct video receiver");return false;}std::lock_guard<std::mutex>l(receiver_mu);receiver=std::move(next);return true;}
    void clear_queue(){std::lock_guard<std::mutex>l(queue_mu);outbound.clear();}
    bool dispatch_control_line(const std::string &line){if(line=="PONG")return true;std::lock_guard<std::mutex>l(receiver_mu);return receiver&&receiver->handle_control_line(line);}
    bool receiver_failed() const{std::lock_guard<std::mutex>l(receiver_mu);return receiver&&receiver->failed();}
    bool recover_control(unsigned long failed,bool video_stall=false){
        std::lock_guard<std::mutex>r(recovery_mu);if(!run.load())return false;if(generation.load()!=failed)return true;
        std::cout<<(video_stall?"Direct video stalled; recovering direct session...\n":"Control interrupted; recovering direct session...\n")<<std::flush;
        stop_receiver();clear_queue();int fd=control_fd.exchange(-1);if(fd>=0)shutdown(fd,SHUT_RDWR);{std::lock_guard<std::mutex>l(control_mu);close_tls(control);}if(!ensure_tunnel()){run.store(false);queue_cv.notify_all();return false;}
        TlsConn next;std::string token,fp;if(!connect_authenticated(false,next,token,fp)){run.store(false);queue_cv.notify_all();return false;}const unsigned long next_generation=failed+1;DirectVideoPath path;if(!negotiate_media(next,token,fp,next_generation,path)){close_tls(next);run.store(false);queue_cv.notify_all();return false;}install_control(next,fp,next_generation);if(!start_receiver(std::move(path))){run.store(false);queue_cv.notify_all();return false;}
        std::cout<<(video_stall?"Direct video restored with fresh keys.\n":"Control restored. Direct video rekeyed.\n")<<std::flush;return true;
    }
    bool write_control(const std::string&line,int timeout){std::lock_guard<std::mutex>l(control_mu);return control.ssl&&tls_write_line_timeout(control.ssl,line,timeout);}
    bool read_control(std::string&line,int timeout){std::lock_guard<std::mutex>l(control_mu);return control.ssl&&tls_read_line_timeout(control.ssl,line,timeout);}
    int current_control_fd(){return control_fd.load();}
    void control_loop(){
        auto next_ping=std::chrono::steady_clock::now()+2s;bool awaiting_pong=false;auto pong_deadline=next_ping;
        while(run.load()){
            unsigned long g=generation.load();
            if(receiver_failed()){set_error("direct video path timed out");if(!recover_control(g,true))break;next_ping=std::chrono::steady_clock::now()+2s;awaiting_pong=false;continue;}
            std::string command;{std::lock_guard<std::mutex>l(queue_mu);if(!outbound.empty()){command=std::move(outbound.front());outbound.pop_front();}}
            if(!command.empty()&&!write_control(command,250)){if(!recover_control(g))break;next_ping=std::chrono::steady_clock::now()+2s;awaiting_pong=false;continue;}
            bool got_input=false;int fd=current_control_fd();if(fd>=0){{std::lock_guard<std::mutex>l(control_mu);if(control.ssl)got_input=tls_line_ready(control.ssl)||SSL_pending(control.ssl)>0;}if(!got_input&&command.empty())got_input=wait_for_input(fd,20);}
            if(got_input){std::string line;if(!read_control(line,250)){if(!recover_control(g))break;next_ping=std::chrono::steady_clock::now()+2s;awaiting_pong=false;continue;}if(line=="PONG")awaiting_pong=false;else if(!dispatch_control_line(line)){set_error("unexpected OPAL control message");if(!recover_control(g))break;next_ping=std::chrono::steady_clock::now()+2s;awaiting_pong=false;continue;}}
            auto now=std::chrono::steady_clock::now();if(awaiting_pong&&now>=pong_deadline){if(!recover_control(g))break;next_ping=std::chrono::steady_clock::now()+2s;awaiting_pong=false;continue;}if(!awaiting_pong&&now>=next_ping){if(!write_control("PING",500)){if(!recover_control(g))break;next_ping=std::chrono::steady_clock::now()+2s;continue;}awaiting_pong=true;pong_deadline=now+1500ms;next_ping=now+2s;}
            if(command.empty()&&!got_input){std::unique_lock<std::mutex>l(queue_mu);queue_cv.wait_for(l,5ms,[&]{return !run.load()||!outbound.empty();});}
        }
    }
    bool start(){
        if(run.load())return true;signal(SIGPIPE,SIG_IGN);ctx=client_tls_context();if(!ctx){set_error("cannot create client TLS context");return false;}run.store(true);generation.store(0);if(!ensure_tunnel()){run.store(false);SSL_CTX_free(ctx);ctx=nullptr;return false;}
        TlsConn initial;std::string token,fp;if(!connect_authenticated(true,initial,token,fp)){run.store(false);tunnel_access_stop(tunnel);SSL_CTX_free(ctx);ctx=nullptr;return false;}DirectVideoPath path;if(!negotiate_media(initial,token,fp,1,path)){close_tls(initial);run.store(false);tunnel_access_stop(tunnel);SSL_CTX_free(ctx);ctx=nullptr;return false;}install_control(initial,fp,1);if(!start_receiver(std::move(path))){run.store(false);{std::lock_guard<std::mutex>l(control_mu);close_tls(control);}tunnel_access_stop(tunnel);SSL_CTX_free(ctx);ctx=nullptr;return false;}control_thread=std::thread([this]{control_loop();});return true;
    }
    void stop(){bool was=run.exchange(false);queue_cv.notify_all();stop_receiver();int fd=control_fd.exchange(-1);if(fd>=0)shutdown(fd,SHUT_RDWR);if(control_thread.joinable())control_thread.join();{std::lock_guard<std::mutex>l(control_mu);close_tls(control);}tunnel_access_stop(tunnel);if(ctx){SSL_CTX_free(ctx);ctx=nullptr;}if(!was)return;}
    bool enqueue(std::string command){if(command.empty())return true;if(!run.load())return false;std::lock_guard<std::mutex>l(queue_mu);if(pointer_command(command)&&!outbound.empty()&&pointer_command(outbound.back())){outbound.back()=std::move(command);queue_cv.notify_one();return true;}if(outbound.size()>=1024){auto it=std::find_if(outbound.begin(),outbound.end(),[](const std::string&s){return pointer_command(s);});if(it!=outbound.end())outbound.erase(it);else{set_error("control queue overflow");run.store(false);int fd=control_fd.load();if(fd>=0)shutdown(fd,SHUT_RDWR);queue_cv.notify_all();return false;}}outbound.push_back(std::move(command));queue_cv.notify_one();return true;}
    bool media_started() const{std::lock_guard<std::mutex>l(receiver_mu);return receiver&&receiver->media_started();}
};

SessionSupervisor::SessionSupervisor(SessionOptions o):impl_(std::make_unique<Impl>(std::move(o))){}
SessionSupervisor::~SessionSupervisor(){impl_->stop();}
bool SessionSupervisor::start(){return impl_->start();}void SessionSupervisor::stop(){impl_->stop();}
bool SessionSupervisor::send_input(const std::string&c){return impl_->enqueue(c);}unsigned long SessionSupervisor::control_generation()const{return impl_->generation.load();}bool SessionSupervisor::media_started()const{return impl_->media_started();}bool SessionSupervisor::running()const{return impl_->run.load();}bool SessionSupervisor::paired()const{return impl_->paired_state.load();}
int SessionSupervisor::remote_width()const{std::lock_guard<std::mutex>l(impl_->state_mu);return impl_->remote_width_value;}int SessionSupervisor::remote_height()const{std::lock_guard<std::mutex>l(impl_->state_mu);return impl_->remote_height_value;}std::string SessionSupervisor::remote_mac()const{std::lock_guard<std::mutex>l(impl_->state_mu);return impl_->remote_mac_value;}std::string SessionSupervisor::fingerprint()const{std::lock_guard<std::mutex>l(impl_->state_mu);return impl_->observed_fingerprint;}std::string SessionSupervisor::last_error()const{std::lock_guard<std::mutex>l(impl_->state_mu);return impl_->error;}
}
