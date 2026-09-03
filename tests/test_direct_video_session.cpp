#include <opal/direct_video_session.hpp>
#include <opal/net.hpp>
#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace fs=std::filesystem;
struct Lines { std::mutex mu;std::condition_variable cv;std::deque<std::string> q; };
static opal::ControlSend sender(Lines &dst){return [&dst](const std::string &line,int){std::lock_guard<std::mutex>l(dst.mu);dst.q.push_back(line);dst.cv.notify_one();return true;};}
static opal::ControlRead reader(Lines &src){return [&src](std::string &line,int timeout){std::unique_lock<std::mutex>l(src.mu);if(!src.cv.wait_for(l,std::chrono::milliseconds(std::max(0,timeout)),[&]{return !src.q.empty();}))return false;line=std::move(src.q.front());src.q.pop_front();return true;};}
static std::uint16_t free_port(){int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);assert(fd>=0);sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=0;assert(bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0);socklen_t n=sizeof(a);assert(getsockname(fd,reinterpret_cast<sockaddr*>(&a),&n)==0);auto p=ntohs(a.sin_port);close(fd);return p;}

int main(){
    auto root=fs::temp_directory_path()/"opal-direct-video-session";fs::remove_all(root);fs::create_directories(root);
    auto cert=(root/"cert.pem").string(),key=(root/"key.pem").string();assert(opal::ensure_tls_certificate(cert,key));
    SSL_CTX *server_ctx=opal::server_tls_context(cert,key),*client_ctx=opal::client_tls_context();assert(server_ctx&&client_ctx);
    auto port=free_port();Lines c2h,h2c;opal::DirectVideoPath host_path,client_path;std::string host_error,client_error;bool host_ok=false;
    std::thread host([&]{int l=opal::listen_tcp(port,"127.0.0.1");assert(l>=0);auto peer=opal::accept_tls(server_ctx,l);assert(peer.ssl);host_ok=opal::negotiate_host_direct_video(peer.ssl,"token","client-pub","host-fp",1,{},sender(h2c),reader(c2h),host_path,host_error,1000);opal::close_tls(peer);close(l);});
    std::this_thread::sleep_for(std::chrono::milliseconds(30));auto client=opal::connect_tls_retry(client_ctx,"127.0.0.1",port,2000,20);assert(client.ssl);
    auto started=std::chrono::steady_clock::now();bool client_ok=opal::negotiate_client_direct_video(client.ssl,"token","client-pub","host-fp",1,{},sender(c2h),reader(h2c),client_path,client_error,1000);auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();opal::close_tls(client);host.join();
    assert(client_ok&&host_ok&&client_error.empty()&&host_error.empty());assert(elapsed<1000);assert(client_path.socket.fd>=0&&host_path.socket.fd>=0);assert(client_path.peer_len>0&&host_path.peer_len>0);assert(client_path.session_id==host_path.session_id);assert(client_path.keys.send_key==host_path.keys.recv_key);

    auto malformed=[&](std::vector<std::string> lines){std::size_t index=0;opal::DirectVideoPath path;std::string error;auto read=[&](std::string &line,int){if(index>=lines.size())return false;line=lines[index++];return true;};auto send=[](const std::string&,int){return true;};return opal::negotiate_client_direct_video(nullptr,"t","p","f",9,{},send,read,path,error,100);};
    assert(!malformed({"UDP_CANDIDATE 8 L ::1 1234"}));
    assert(!malformed({"UDP_CANDIDATE 9 L ::1 0"}));
    assert(!malformed({"UDP_CANDIDATE 9 L ::1 1234 extra"}));
    assert(!malformed({"UDP_CANDIDATE 9 L "+std::string(256,'a')+" 1234"}));
    std::vector<std::string> too_many;for(int i=0;i<17;++i)too_many.push_back("UDP_CANDIDATE 9 L ::1 "+std::to_string(2000+i));too_many.push_back("UDP_CANDIDATES_DONE 9");assert(!malformed(too_many));

    SSL_CTX_free(client_ctx);SSL_CTX_free(server_ctx);fs::remove_all(root);return 0;
}
