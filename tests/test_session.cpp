#include <opal/session.hpp>
#include <opal/crypto.hpp>
#include <opal/direct_video_session.hpp>
#include <opal/net.hpp>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

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
    while(std::chrono::steady_clock::now()<end){if(pred())return true;std::this_thread::sleep_for(25ms);}
    return pred();
}

static std::string cert_fingerprint(const std::string&path){
    FILE*f=std::fopen(path.c_str(),"r");assert(f);
    X509*x=PEM_read_X509(f,nullptr,nullptr,nullptr);std::fclose(f);assert(x);
    unsigned char md[EVP_MAX_MD_SIZE];unsigned int n=0;assert(X509_digest(x,EVP_sha256(),md,&n)==1);
    X509_free(x);return opal::hex(md,n);
}

static std::uint32_t parse_generation(const std::string&line){
    std::istringstream in(line);std::string word,extra;unsigned long long generation=0;
    assert(in>>word>>generation);assert(word=="UDP_GENERATION");assert(!(in>>extra));assert(generation>0&&generation<=0xffffffffULL);
    return static_cast<std::uint32_t>(generation);
}

static void assert_profile(const std::string&line,std::uint32_t generation){
    std::istringstream in(line);std::string word,extra;unsigned long long got=0;int width=0,height=0,fps=0;
    assert(in>>word>>got>>width>>height>>fps);assert(!(in>>extra));
    assert(word=="VIDEO_PROFILE"&&got==generation);assert(width==1920&&height==1080&&fps==60);
}

int main(){
    auto root=fs::temp_directory_path()/"opal-session-supervisor-test";
    fs::remove_all(root);fs::create_directories(root);setenv("OPAL_DISABLE_STUN","1",1);
    auto cert=(root/"cert.pem").string(),key=(root/"key.pem").string();assert(opal::ensure_tls_certificate(cert,key));
    const auto server_fp=cert_fingerprint(cert);
    auto priv=root/"client.key",pub=root/"client.pub";assert(opal::ensure_identity(priv,pub));
    auto public_hex=opal::public_key_hex(pub);assert(!public_hex.empty());
    SSL_CTX*server_ctx=opal::server_tls_context(cert,key);assert(server_ctx);
    uint16_t control_port=free_port();
    std::atomic<bool> run{true};
    std::atomic<int> pair_count{0},auth_count{0},control_accepts{0},direct_paths{0},receiver_ready{0},input_count{0};
    std::mutex ids_mu;std::vector<std::uint64_t> session_ids;

    std::thread control_server([&]{
        int lfd=opal::listen_tcp(control_port,"127.0.0.1");assert(lfd>=0);
        int accepted_generation=0;
        while(run.load()&&accepted_generation<3){
            pollfd p{lfd,POLLIN,0};if(poll(&p,1,100)<=0)continue;
            auto c=opal::accept_tls(server_ctx,lfd);if(!c.ssl)continue;
            ++accepted_generation;++control_accepts;
            const int expected_generation=accepted_generation;
            std::string nonce="nonce-"+std::to_string(expected_generation);
            std::string challenge=nonce+" 1920 1080 00:11:22:33:44:55";
            assert(opal::tls_write_line(c.ssl,"CHALLENGE OPAL3 "+challenge));
            std::string line;assert(opal::tls_read_line_timeout(c.ssl,line,3000));
            std::istringstream auth(line);std::string mode,pubkey,proof;auth>>mode>>pubkey>>proof;assert(pubkey==public_hex);
            if(expected_generation==1){
                assert(mode=="PAIR");
                auto transcript="OPAL-PAIR-v3\n"+server_fp+"\n"+challenge+"\n"+pubkey;
                assert(proof==opal::hmac_sha256_hex("test-password",transcript));++pair_count;
            }else{
                assert(mode=="AUTH");assert(opal::verify_hex(pubkey,challenge,proof));++auth_count;
            }
            const std::string token="session-token-"+std::to_string(expected_generation);
            assert(opal::tls_write_line(c.ssl,"OK "+token));
            assert(opal::tls_read_line_timeout(c.ssl,line,2000));
            const auto generation=parse_generation(line);assert(generation==static_cast<std::uint32_t>(expected_generation));
            assert(opal::tls_read_line_timeout(c.ssl,line,2000));assert_profile(line,generation);

            opal::DirectVideoPath path;std::string error;
            auto send=[&](const std::string&message,int timeout){
                const bool ok=opal::tls_write_line_timeout(c.ssl,message,timeout);
                std::cerr<<"HOST["<<generation<<"] -> "<<message<<" result="<<ok<<" timeout="<<timeout<<"ms\n";
                return ok;
            };
            auto read=[&](std::string&message,int timeout){
                const bool ok=opal::tls_read_line_timeout(c.ssl,message,timeout);
                std::cerr<<"HOST["<<generation<<"] <- "<<(ok?message:"<timeout-or-closed>")<<" result="<<ok<<" timeout="<<timeout<<"ms\n";
                return ok;
            };
            if(!opal::negotiate_host_direct_video(c.ssl,token,pubkey,server_fp,generation,{},send,read,path,error,5000)){
                std::cerr<<"HOST["<<generation<<"] direct negotiation failed: "<<error<<"\n";
                std::abort();
            }
            assert(path.generation==generation&&path.session_id!=0&&path.socket.fd>=0&&path.peer_len>0);
            {std::lock_guard<std::mutex> lock(ids_mu);session_ids.push_back(path.session_id);}++direct_paths;

            assert(opal::tls_read_line_timeout(c.ssl,line,2000));
            assert(line=="DIRECT_RECEIVER_READY "+std::to_string(generation));++receiver_ready;
            assert(opal::tls_write_line(c.ssl,"DIRECT_MEDIA_READY "+std::to_string(generation)));

            while(run.load()){
                if(!opal::tls_read_line_timeout(c.ssl,line,1000))break;
                if(line=="PING"){
                    if(expected_generation==1){opal::close_tls(c);break;}
                    assert(opal::tls_write_line(c.ssl,"PONG"));continue;
                }
                if(line=="MOUSE 1 2"){++input_count;continue;}
                if(line.rfind("VIDEO_FEEDBACK ",0)==0||line.rfind("CLOCK_SYNC ",0)==0||line.rfind("REQUEST_IDR ",0)==0)continue;
                assert(false&&"unexpected control line");
            }
            opal::close_tls(c);
        }
        close(lfd);
    });

    opal::SessionOptions options;
    options.target="127.0.0.1";options.control_port=control_port;options.client_public_key=public_hex;
    options.client_private_key_path=priv.string();options.pairing_password="test-password";options.paired=false;
    opal::SessionSupervisor session(options);
    assert(session.start());
    assert(session.remote_width()==1920);assert(session.remote_height()==1080);assert(session.remote_mac()=="00:11:22:33:44:55");
    assert(!session.media_started());
    assert(wait_until([&]{return session.control_generation()>=2;},9000));
    assert(session.running());assert(session.remote_width()==1920&&session.remote_height()==1080);assert(session.remote_mac()=="00:11:22:33:44:55");
    assert(pair_count.load()==1);assert(auth_count.load()>=1);assert(control_accepts.load()>=2);assert(direct_paths.load()>=2);assert(receiver_ready.load()>=2);
    {std::lock_guard<std::mutex> lock(ids_mu);assert(session_ids.size()>=2);assert(session_ids[0]!=session_ids[1]);}
    auto input_started=std::chrono::steady_clock::now();assert(session.send_input("MOUSE 1 2"));
    auto input_elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-input_started).count();assert(input_elapsed<100);
    assert(wait_until([&]{return input_count.load()>=1;},2000));
    session.stop();run.store(false);control_server.join();SSL_CTX_free(server_ctx);unsetenv("OPAL_DISABLE_STUN");fs::remove_all(root);
    return 0;
}
