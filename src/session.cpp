#include <opal/session.hpp>
#include <opal/crypto.hpp>
#include <opal/net.hpp>
#include <opal/tunnel.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace opal {
namespace {
using namespace std::chrono_literals;

bool debug_enabled(){
    const char*v=std::getenv("OPAL_DEBUG");
    if(!v||!*v)return false;
    std::string x=v;
    return x!="0"&&x!="false"&&x!="FALSE"&&x!="off"&&x!="OFF";
}

std::string player_command(){
    if(const char*e=std::getenv("OPAL_PLAYER_CMD");e&&*e)return e;
    std::string cmd=debug_enabled()
        ?"ffplay -hide_banner -loglevel warning -flags low_delay -framedrop -fs -autoexit -i pipe:0"
        :"ffplay -hide_banner -loglevel quiet -flags low_delay -framedrop -fs -autoexit -i pipe:0 >/dev/null 2>&1";
    if(const char*display=std::getenv("DISPLAY");display&&*display)cmd="SDL_VIDEODRIVER=x11 "+cmd;
    return cmd;
}

bool wait_for_input(int fd,int timeout_ms){
    pollfd p{fd,POLLIN,0};
    for(;;){
        int rc=poll(&p,1,timeout_ms);
        if(rc<0&&errno==EINTR)continue;
        if(rc<=0)return false;
        if(p.revents&(POLLERR|POLLHUP|POLLNVAL))return false;
        return (p.revents&POLLIN)!=0;
    }
}
}

struct SessionSupervisor::Impl {
    explicit Impl(SessionOptions o):options(std::move(o)),paired_state(options.paired){}

    SessionOptions options;
    SSL_CTX*ctx=nullptr;
    TlsConn control;
    std::mutex control_mu;
    std::mutex recovery_mu;
    mutable std::mutex state_mu;
    std::condition_variable state_cv;
    std::thread heartbeat_thread;
    std::thread video_thread;
    std::atomic<bool> run{false};
    std::atomic<bool> media{false};
    std::atomic<unsigned long> generation{0};
    std::atomic<int> control_fd{-1};
    std::atomic<int> video_fd{-1};
    std::string current_video_token;
    std::string observed_fingerprint;
    std::string error;
    bool paired_state=false;

    void set_error(const std::string&message){
        std::lock_guard<std::mutex>l(state_mu);
        error=message;
    }

    bool ensure_tunnel(){
        if(!options.tunneled)return true;
        if(tunnel_access(options.control_token,options.video_token))return true;
        set_error("OPAL tunnel access failed");
        return false;
    }

    bool verify_fingerprint(TlsConn&c,std::string&fp){
        fp=peer_fingerprint(c.ssl);
        std::string expected;
        {
            std::lock_guard<std::mutex>l(state_mu);
            expected=!options.fingerprint.empty()?options.fingerprint:observed_fingerprint;
        }
        if(!expected.empty()&&!secure_equal(expected,fp)){
            set_error("host certificate changed; refusing connection");
            return false;
        }
        return true;
    }

    bool authenticate(TlsConn&c,bool allow_pair,std::string&token,std::string&fp){
        if(!verify_fingerprint(c,fp))return false;
        std::string challenge;
        if(!tls_read_line_timeout(c.ssl,challenge,5000)||challenge.rfind("CHALLENGE ",0)!=0){
            set_error("control authentication challenge failed");
            return false;
        }
        auto nonce=challenge.substr(10);
        std::string auth;
        if(paired_state){
            if(options.client_public_key.empty()||options.client_private_key_path.empty()){
                set_error("saved client identity unavailable");
                return false;
            }
            auto signature=sign_hex(std::filesystem::path(options.client_private_key_path),nonce);
            if(signature.empty()){
                set_error("client authentication signature failed");
                return false;
            }
            auth="AUTH "+options.client_public_key+" "+signature;
        }else{
            if(!allow_pair){
                set_error("paired recovery cannot fall back to pairing");
                return false;
            }
            std::string password=options.pairing_password;
            if(password.empty()&&options.pairing_password_provider)password=options.pairing_password_provider();
            if(password.empty()){
                set_error("pairing password required");
                return false;
            }
            auth="PAIR "+options.client_public_key+" "+hmac_sha256_hex(password,nonce+options.client_public_key);
            if(!options.label.empty())auth+=" "+options.label;
        }
        if(!tls_write_line(c.ssl,auth)){
            set_error("control authentication write failed");
            return false;
        }
        std::string reply;
        if(!tls_read_line_timeout(c.ssl,reply,5000)||reply.rfind("OK ",0)!=0){
            set_error("authentication denied");
            return false;
        }
        token=reply.substr(3);
        if(token.empty()){
            set_error("host returned an empty video token");
            return false;
        }
        if(!paired_state)paired_state=true;
        return true;
    }

    bool connect_authenticated(bool allow_pair,TlsConn&out,std::string&token,std::string&fp){
        auto c=connect_tls_retry(ctx,options.target,static_cast<uint16_t>(options.control_port),30000,100);
        if(!c.ssl){set_error("cannot connect to OPAL host");return false;}
        if(!authenticate(c,allow_pair,token,fp)){close_tls(c);return false;}
        out=c;c={};
        return true;
    }

    void install_control(TlsConn&next,const std::string&token,const std::string&fp,bool first){
        {
            std::lock_guard<std::mutex>l(control_mu);
            close_tls(control);
            control=next;next={};
            control_fd.store(control.fd);
        }
        {
            std::lock_guard<std::mutex>l(state_mu);
            current_video_token=token;
            if(observed_fingerprint.empty())observed_fingerprint=fp;
            if(options.fingerprint.empty())options.fingerprint=observed_fingerprint;
            error.clear();
        }
        if(first)generation.store(1);
        else generation.fetch_add(1);
        state_cv.notify_all();
    }

    void interrupt_video(){
        int fd=video_fd.load();
        if(fd>=0)shutdown(fd,SHUT_RDWR);
    }

    bool recover_control(unsigned long failed_generation){
        std::lock_guard<std::mutex>recover(recovery_mu);
        if(!run.load())return false;
        if(generation.load()!=failed_generation)return true;
        interrupt_video();
        int fd=control_fd.exchange(-1);
        if(fd>=0)shutdown(fd,SHUT_RDWR);
        {
            std::lock_guard<std::mutex>l(control_mu);
            close_tls(control);
        }
        std::cout<<"Control interrupted; recovering...\n"<<std::flush;
        if(!ensure_tunnel()){
            run.store(false);state_cv.notify_all();return false;
        }
        TlsConn next;std::string token,fp;
        // Once an initial connection has paired successfully, recovery is AUTH-only.
        if(!paired_state||!connect_authenticated(false,next,token,fp)){
            if(!paired_state)set_error("control recovery requires a paired identity");
            run.store(false);state_cv.notify_all();return false;
        }
        install_control(next,token,fp,false);
        std::cout<<"Control restored.\n"<<std::flush;
        return true;
    }

    void heartbeat(){
        while(run.load()){
            for(int i=0;i<20&&run.load();++i)std::this_thread::sleep_for(100ms);
            if(!run.load())break;
            unsigned long g=generation.load();
            bool ok=false;
            {
                std::lock_guard<std::mutex>l(control_mu);
                if(control.ssl&&tls_write_line(control.ssl,"PING")){
                    std::string pong;
                    ok=tls_read_line_timeout(control.ssl,pong,5000)&&pong=="PONG";
                }
            }
            if(!ok&&!recover_control(g))break;
        }
    }

    bool open_video(TlsConn&v,const std::string&token){
        auto next=connect_tls_retry(ctx,options.target,static_cast<uint16_t>(options.video_port),10000,100);
        if(!next.ssl)return false;
        std::string fp=peer_fingerprint(next.ssl);
        std::string expected;
        {std::lock_guard<std::mutex>l(state_mu);expected=observed_fingerprint;}
        if(!expected.empty()&&!secure_equal(expected,fp)){close_tls(next);set_error("video certificate changed; refusing connection");return false;}
        if(!tls_write_line(next.ssl,"VIDEO "+token)){close_tls(next);return false;}
        std::string ready;
        if(!tls_read_line_timeout(next.ssl,ready,12000)||ready!="READY"){close_tls(next);return false;}
        v=next;next={};
        return true;
    }

    void video_loop(){
        bool recovering=false;
        bool first_attempt=true;
        unsigned long last_control_generation=0;
        while(run.load()){
            std::string token;
            unsigned long g=0;
            {
                std::unique_lock<std::mutex>l(state_mu);
                state_cv.wait_for(l,200ms,[&]{return !run.load()||(!current_video_token.empty()&&generation.load()>0);});
                if(!run.load())break;
                token=current_video_token;
                g=generation.load();
            }
            if(token.empty())continue;
            if(first_attempt){std::cout<<"Video connecting...\n"<<std::flush;first_attempt=false;}
            TlsConn v;
            if(!open_video(v,token)){
                if(run.load())std::this_thread::sleep_for(150ms);
                continue;
            }
            video_fd.store(v.fd);
            FILE*player=popen(player_command().c_str(),"w");
            if(!player){video_fd.store(-1);close_tls(v);set_error("could not start video player");std::this_thread::sleep_for(150ms);continue;}
            if(recovering){std::cout<<"Video restored.\n"<<std::flush;recovering=false;}
            else if(!media.load())std::cout<<"Video connected.\n"<<std::flush;
            last_control_generation=g;
            bool got_media=false;
            bool stream_ok=true;
            while(run.load()&&generation.load()==g){
                if(SSL_pending(v.ssl)==0&&!wait_for_input(v.fd,5000)){stream_ok=false;break;}
                char buf[65536];
                int n=SSL_read(v.ssl,buf,sizeof(buf));
                if(n<=0){stream_ok=false;break;}
                if(std::fwrite(buf,1,static_cast<size_t>(n),player)!=static_cast<size_t>(n)||std::fflush(player)!=0){stream_ok=false;break;}
                got_media=true;media.store(true);
            }
            video_fd.store(-1);
            shutdown(v.fd,SHUT_RDWR);
            close_tls(v);
            pclose(player);
            if(!run.load())break;
            if(got_media||!stream_ok||generation.load()!=last_control_generation){
                std::cout<<"Video interrupted; recovering...\n"<<std::flush;
                // Preserve the previous diagnostic during the transition so existing scripts remain compatible.
                std::cout<<"Video stalled; reconnecting...\n"<<std::flush;
                recovering=true;
            }
            std::this_thread::sleep_for(100ms);
        }
        video_fd.store(-1);
    }

    bool start(){
        if(run.load())return true;
        signal(SIGPIPE,SIG_IGN);
        ctx=client_tls_context();
        if(!ctx){set_error("cannot create client TLS context");return false;}
        run.store(true);media.store(false);generation.store(0);
        if(!ensure_tunnel()){run.store(false);SSL_CTX_free(ctx);ctx=nullptr;return false;}
        TlsConn initial;std::string token,fp;
        if(!connect_authenticated(true,initial,token,fp)){
            run.store(false);SSL_CTX_free(ctx);ctx=nullptr;return false;
        }
        install_control(initial,token,fp,true);
        heartbeat_thread=std::thread([this]{heartbeat();});
        video_thread=std::thread([this]{video_loop();});
        return true;
    }

    void stop(){
        bool was_running=run.exchange(false);
        state_cv.notify_all();
        interrupt_video();
        int fd=control_fd.exchange(-1);
        if(fd>=0)shutdown(fd,SHUT_RDWR);
        if(heartbeat_thread.joinable())heartbeat_thread.join();
        if(video_thread.joinable())video_thread.join();
        {
            std::lock_guard<std::mutex>l(control_mu);
            close_tls(control);
        }
        if(ctx){SSL_CTX_free(ctx);ctx=nullptr;}
        if(!was_running)return;
    }
};

SessionSupervisor::SessionSupervisor(SessionOptions options):impl_(std::make_unique<Impl>(std::move(options))){}
SessionSupervisor::~SessionSupervisor(){impl_->stop();}
bool SessionSupervisor::start(){return impl_->start();}
void SessionSupervisor::stop(){impl_->stop();}
bool SessionSupervisor::send_input(const std::string&command){
    if(command.empty())return true;
    if(!impl_->run.load())return false;
    unsigned long g=impl_->generation.load();
    {
        std::lock_guard<std::mutex>l(impl_->control_mu);
        if(impl_->control.ssl&&tls_write_line(impl_->control.ssl,command))return true;
    }
    // Do not replay the event that discovered the dead generation. A key/button
    // event replayed after AUTH could become stuck because the old generation's
    // held-input state is deliberately discarded during recovery.
    return impl_->recover_control(g);
}
unsigned long SessionSupervisor::control_generation()const{return impl_->generation.load();}
bool SessionSupervisor::media_started()const{return impl_->media.load();}
bool SessionSupervisor::running()const{return impl_->run.load();}
bool SessionSupervisor::paired()const{return impl_->paired_state;}
std::string SessionSupervisor::fingerprint()const{std::lock_guard<std::mutex>l(impl_->state_mu);return impl_->observed_fingerprint;}
std::string SessionSupervisor::last_error()const{std::lock_guard<std::mutex>l(impl_->state_mu);return impl_->error;}
}
