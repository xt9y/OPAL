#include <opal/direct_video_session.hpp>
#include <opal/net.hpp>
#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>
#include <vector>
#include <unistd.h>

namespace fs=std::filesystem;
static std::uint16_t free_tcp_port(){int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);assert(fd>=0);sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=0;assert(bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0);socklen_t n=sizeof(a);assert(getsockname(fd,reinterpret_cast<sockaddr*>(&a),&n)==0);auto p=ntohs(a.sin_port);close(fd);return p;}
static bool with_tls_client(SSL_CTX *server_ctx,SSL_CTX *client_ctx,const std::function<bool(SSL*)> &body){auto port=free_tcp_port();std::thread server([&]{int listener=opal::listen_tcp(port,"127.0.0.1");assert(listener>=0);auto peer=opal::accept_tls(server_ctx,listener);assert(peer.ssl);std::this_thread::sleep_for(std::chrono::milliseconds(600));opal::close_tls(peer);close(listener);});std::this_thread::sleep_for(std::chrono::milliseconds(20));auto client=opal::connect_tls_retry(client_ctx,"127.0.0.1",port,1000,20);assert(client.ssl);bool result=body(client.ssl);opal::close_tls(client);server.join();return result;}

int main(){
    auto root=fs::temp_directory_path()/"opal-direct-video-security";fs::remove_all(root);fs::create_directories(root);auto cert=(root/"cert.pem").string(),key=(root/"key.pem").string();assert(opal::ensure_tls_certificate(cert,key));SSL_CTX *server_ctx=opal::server_tls_context(cert,key),*client_ctx=opal::client_tls_context();assert(server_ctx&&client_ctx);
    int attacker=socket(AF_INET6,SOCK_DGRAM|SOCK_CLOEXEC,0);assert(attacker>=0);sockaddr_in6 addr{};addr.sin6_family=AF_INET6;addr.sin6_addr=in6addr_loopback;addr.sin6_port=0;assert(bind(attacker,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0);socklen_t alen=sizeof(addr);assert(getsockname(attacker,reinterpret_cast<sockaddr*>(&addr),&alen)==0);auto attacker_port=ntohs(addr.sin6_port);
    std::thread attack([&]{std::uint8_t wire[1201]{};sockaddr_storage peer{};socklen_t peer_len=sizeof(peer);ssize_t n=recvfrom(attacker,wire,sizeof(wire),0,reinterpret_cast<sockaddr*>(&peer),&peer_len);if(n>52){wire[5]=static_cast<std::uint8_t>(opal::VideoMediaType::ProbeAck);sendto(attacker,wire,n,0,reinterpret_cast<sockaddr*>(&peer),peer_len);}close(attacker);});
    bool forged=with_tls_client(server_ctx,client_ctx,[&](SSL *ssl){std::vector<std::string> lines={"UDP_CANDIDATE 2 L ::1 "+std::to_string(attacker_port),"UDP_CANDIDATES_DONE 2","UDP_PROBE_READY 2"};std::size_t index=0;auto read=[&](std::string &line,int timeout){if(index<lines.size()){line=lines[index++];return true;}std::this_thread::sleep_for(std::chrono::milliseconds(timeout));return false;};opal::DirectVideoPath path;std::string error;auto started=std::chrono::steady_clock::now();bool ok=opal::negotiate_client_direct_video(ssl,"token","pub","fp",2,{},[](const std::string&,int){return true;},read,path,error,250);auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();assert(!ok&&error==opal::direct_video_unavailable_error()&&elapsed<600);return ok;});assert(!forged);attack.join();
    bool unreachable=with_tls_client(server_ctx,client_ctx,[&](SSL *ssl){std::vector<std::string> lines={"UDP_CANDIDATE 3 L ::1 9","UDP_CANDIDATES_DONE 3","UDP_PROBE_READY 3"};std::size_t index=0;auto read=[&](std::string &line,int timeout){if(index<lines.size()){line=lines[index++];return true;}std::this_thread::sleep_for(std::chrono::milliseconds(timeout));return false;};opal::DirectVideoPath path;std::string error;bool ok=opal::negotiate_client_direct_video(ssl,"token","pub","fp",3,{},[](const std::string&,int){return true;},read,path,error,250);assert(!ok&&error==opal::direct_video_unavailable_error());return ok;});assert(!unreachable);
    SSL_CTX_free(client_ctx);SSL_CTX_free(server_ctx);fs::remove_all(root);return 0;
}
