#include <opal/session.hpp>
#include <opal/crypto.hpp>
#include <opal/net.hpp>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>

namespace fs=std::filesystem;
using namespace std::chrono_literals;

static uint16_t free_port(){
    int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);
    sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=0;
    assert(bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0);
    socklen_t n=sizeof(a);assert(getsockname(fd,reinterpret_cast<sockaddr*>(&a),&n)==0);
    auto p=ntohs(a.sin_port);close(fd);return p;
}

static bool wait_until(const std::function<bool()>&pred,int timeout_ms){
    auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout_ms);
    while(std::chrono::steady_clock::now()<end){if(pred())return true;std::this_thread::sleep_for(25ms);}return pred();
}

static int line_count(const fs::path&p){std::ifstream in(p);int n=0;std::string s;while(std::getline(in,s))++n;return n;}
static std::string read_all(const fs::path&p){std::ifstream in(p);return std::string((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());}
static std::string cert_fingerprint(const std::string&path){
    FILE*f=std::fopen(path.c_str(),"r");assert(f);
    X509*x=PEM_read_X509(f,nullptr,nullptr,nullptr);std::fclose(f);assert(x);
    unsigned char md[EVP_MAX_MD_SIZE];unsigned int n=0;assert(X509_digest(x,EVP_sha256(),md,&n)==1);X509_free(x);
    return opal::hex(md,n);
}

int main(){
    auto root=fs::temp_directory_path()/"opal-session-supervisor-test";
    fs::remove_all(root);fs::create_directories(root);
    auto cert=(root/"cert.pem").string(),key=(root/"key.pem").string();
    assert(opal::ensure_tls_certificate(cert,key));
    const auto server_fp=cert_fingerprint(cert);
    auto priv=root/"client.key",pub=root/"client.pub";assert(opal::ensure_identity(priv,pub));
    auto public_hex=opal::public_key_hex(pub);assert(!public_hex.empty());

    auto player_log=root/"players.log";
    auto bin=root/"bin";fs::create_directories(bin);
    auto player=bin/"ffplay";
    {
        std::ofstream out(player);
        out<<"#!/bin/sh\n"
              "printf '%s\\n' \"$*\" >> \"$OPAL_TEST_PLAYER_LOG\"\n"
              "dd bs=1 count=128 of=/dev/null 2>/dev/null\n";
    }
    chmod(player.c_str(),0755);
    std::string old_path=std::getenv("PATH")?std::getenv("PATH"):"";
    std::string test_path=bin.string()+":"+old_path;
    setenv("PATH",test_path.c_str(),1);
    unsetenv("OPAL_PLAYER_CMD");
    setenv("OPAL_TEST_PLAYER_LOG",player_log.c_str(),1);

    SSL_CTX*server_ctx=opal::server_tls_context(cert,key);assert(server_ctx);
    uint16_t control_port=free_port(),video_port=free_port();
    std::atomic<bool> run{true};
    std::atomic<int> pair_count{0},auth_count{0},control_accepts{0},video_accepts{0};
    std::mutex tokens_mu;std::vector<std::string> video_tokens;

    std::thread control_server([&]{
        int lfd=opal::listen_tcp(control_port,"127.0.0.1");assert(lfd>=0);
        int generation=0;
        while(run.load()&&generation<4){
            pollfd p{lfd,POLLIN,0};if(poll(&p,1,100)<=0)continue;
            auto c=opal::accept_tls(server_ctx,lfd);if(!c.ssl)continue;
            ++generation;++control_accepts;
            std::string nonce="nonce-"+std::to_string(generation);
            std::string challenge=nonce+" 1920 1080 00:11:22:33:44:55";
            assert(opal::tls_write_line(c.ssl,"CHALLENGE OPAL2 "+challenge));
            std::string line;assert(opal::tls_read_line_timeout(c.ssl,line,3000));
            if(generation==1){
                std::istringstream auth(line);std::string mode,pubkey,proof;auth>>mode>>pubkey>>proof;
                assert(mode=="PAIR");assert(pubkey==public_hex);
                auto transcript="OPAL-PAIR-v2\n"+server_fp+"\n"+challenge+"\n"+pubkey;
                assert(proof==opal::hmac_sha256_hex("test-password",transcript));
                ++pair_count;
            }else{assert(line.rfind("AUTH ",0)==0);++auth_count;}
            std::string token="video-token-"+std::to_string(generation);
            assert(opal::tls_write_line(c.ssl,"OK "+token));
            while(run.load()){
                pollfd peer{c.fd,POLLIN,0};
                int ready=poll(&peer,1,3000);
                if(ready<0)break;
                if(ready==0)continue;
                if(peer.revents&(POLLERR|POLLHUP|POLLNVAL))break;
                if(!(peer.revents&POLLIN))continue;
                if(!opal::tls_read_line_timeout(c.ssl,line,500))break;
                if(line=="PING"){
                    if(generation==1){opal::close_tls(c);break;}
                    assert(opal::tls_write_line(c.ssl,"PONG"));
                }
            }
            opal::close_tls(c);
        }
        close(lfd);
    });

    std::thread video_server([&]{
        int lfd=opal::listen_tcp(video_port,"127.0.0.1");assert(lfd>=0);
        while(run.load()){
            pollfd p{lfd,POLLIN,0};if(poll(&p,1,100)<=0)continue;
            auto c=opal::accept_tls(server_ctx,lfd);if(!c.ssl)continue;
            std::string line;if(!opal::tls_read_line_timeout(c.ssl,line,3000)){opal::close_tls(c);continue;}
            assert(line.rfind("VIDEO ",0)==0);
            {std::lock_guard<std::mutex>l(tokens_mu);video_tokens.push_back(line.substr(6));}
            ++video_accepts;
            assert(opal::tls_write_line(c.ssl,"READY"));
            std::string payload(4096,'V');
            for(int i=0;i<200&&run.load();++i){
                if(!opal::tls_write_all_timeout(c.ssl,payload.data(),payload.size(),500))break;
                std::this_thread::sleep_for(5ms);
            }
            opal::close_tls(c);
        }
        close(lfd);
    });

    opal::SessionOptions options;
    options.target="127.0.0.1";
    options.control_port=control_port;
    options.video_port=video_port;
    options.client_public_key=public_hex;
    options.client_private_key_path=priv.string();
    options.pairing_password="test-password";
    options.paired=false;
    opal::SessionSupervisor session(options);
    assert(session.start());
    assert(session.remote_width()==1920);
    assert(session.remote_height()==1080);
    assert(session.remote_mac()=="00:11:22:33:44:55");
    assert(wait_until([&]{return session.media_started();},5000));
    assert(wait_until([&]{return line_count(player_log)>=2;},5000));
    assert(wait_until([&]{return session.control_generation()>=2;},9000));
    assert(session.remote_width()==1920);
    assert(session.remote_height()==1080);
    assert(session.remote_mac()=="00:11:22:33:44:55");
    assert(pair_count.load()==1);
    assert(auth_count.load()>=1);
    assert(control_accepts.load()>=2);
    assert(wait_until([&]{
        std::lock_guard<std::mutex>l(tokens_mu);
        bool first=false,second=false;
        for(auto&t:video_tokens){first|=t=="video-token-1";second|=t=="video-token-2";}
        return first&&second;
    },5000));
    assert(video_accepts.load()>=3);
    auto input_started=std::chrono::steady_clock::now();
    assert(session.send_input("MOUSE 1 2"));
    auto input_elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-input_started).count();
    assert(input_elapsed<100);

    session.stop();run.store(false);
    control_server.join();video_server.join();SSL_CTX_free(server_ctx);
    auto player_args=read_all(player_log);
    assert(!player_args.empty());
    assert(player_args.find("-fflags nobuffer")==std::string::npos);
    setenv("PATH",old_path.c_str(),1);
    unsetenv("OPAL_TEST_PLAYER_LOG");
    fs::remove_all(root);
    return 0;
}
