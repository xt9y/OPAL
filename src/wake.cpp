#include <opal/wake.hpp>
#include <opal/config.hpp>
#include <opal/crypto.hpp>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

namespace opal { namespace {
int tcp_socket(){
#ifdef SOCK_CLOEXEC
    int cloexec_fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);if(cloexec_fd>=0)return cloexec_fd;
#endif
    int fd=socket(AF_INET,SOCK_STREAM,0);if(fd>=0){int flags=fcntl(fd,F_GETFD,0);if(flags>=0)fcntl(fd,F_SETFD,flags|FD_CLOEXEC);}return fd;
}
int accept_cloexec(int fd){
#ifdef __linux__
    int cloexec_fd=accept4(fd,nullptr,nullptr,SOCK_CLOEXEC);if(cloexec_fd>=0)return cloexec_fd;if(errno!=ENOSYS&&errno!=EINVAL)return -1;
#endif
    int c=accept(fd,nullptr,nullptr);if(c>=0){int flags=fcntl(c,F_GETFD,0);if(flags>=0)fcntl(c,F_SETFD,flags|FD_CLOEXEC);}return c;
}
void socket_deadlines(int fd,int seconds=3){timeval tv{seconds,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));}
bool connect_deadline(int fd,const sockaddr_in&a,int timeout_ms){int flags=fcntl(fd,F_GETFL,0);if(flags<0)return false;if(fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0)return false;int rc=connect(fd,reinterpret_cast<const sockaddr*>(&a),sizeof(a));if(rc==0){fcntl(fd,F_SETFL,flags);return true;}if(errno!=EINPROGRESS){fcntl(fd,F_SETFL,flags);return false;}pollfd p{fd,POLLOUT,0};do{rc=poll(&p,1,timeout_ms);}while(rc<0&&errno==EINTR);int err=0;socklen_t n=sizeof(err);bool ok=rc>0&&!(p.revents&(POLLERR|POLLHUP|POLLNVAL))&&getsockopt(fd,SOL_SOCKET,SO_ERROR,&err,&n)==0&&err==0;fcntl(fd,F_SETFL,flags);return ok;}
bool send_all(int fd,const std::string&s){size_t pos=0;while(pos<s.size()){ssize_t n=send(fd,s.data()+pos,s.size()-pos,MSG_NOSIGNAL);if(n>0){pos+=static_cast<size_t>(n);continue;}if(n<0&&errno==EINTR)continue;return false;}return true;}
void bridge_peer(int c,const std::string&secret,const std::string&mac){socket_deadlines(c);auto nonce=random_hex(24);if(!send_all(c,nonce+"\n")){close(c);return;}char buf[512]{};ssize_t n;do{n=recv(c,buf,sizeof(buf)-1,0);}while(n<0&&errno==EINTR);std::string got(buf,n>0?static_cast<size_t>(n):0);got=trim(got);if(n>0&&secure_equal(got,hmac_sha256_hex(secret,nonce))){bool ok=send_wol(mac);send_all(c,ok?"OK\n":"ERR\n");}else send_all(c,"DENY\n");close(c);}
}

std::vector<uint8_t>wol_packet(const std::string&mac){std::array<unsigned,6>b{};char c;std::istringstream ss(mac);for(int i=0;i<6;i++){if(!(ss>>std::hex>>b[i])||b[i]>0xff)return{};if(i<5&&(!(ss>>c)||c!=':'))return{};}std::vector<uint8_t>p(102,0xff);for(int r=0;r<16;r++)for(int i=0;i<6;i++)p[6+r*6+i]=static_cast<uint8_t>(b[i]);return p;}
bool send_wol(const std::string&mac,const std::string&broadcast,uint16_t port){auto p=wol_packet(mac);if(p.empty())return false;int fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);if(fd<0)return false;int yes=1;setsockopt(fd,SOL_SOCKET,SO_BROADCAST,&yes,sizeof(yes));sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);if(inet_pton(AF_INET,broadcast.c_str(),&a.sin_addr)!=1){close(fd);return false;}auto n=sendto(fd,p.data(),p.size(),MSG_NOSIGNAL,reinterpret_cast<sockaddr*>(&a),sizeof(a));close(fd);return n==static_cast<ssize_t>(p.size());}
int run_bridge(uint16_t port){Paths p=Paths::load();ensure_layout(p);Ini cfg;if(!cfg.load(p.root/"bridge.ini")){std::cerr<<"bridge not configured; run: opal bridge setup --mac XX:XX:XX:XX:XX:XX\n";return 2;}auto secret=cfg.get("bridge","secret"),mac=cfg.get("bridge","mac");if(secret.empty()||secret.size()<32||wol_packet(mac).empty()){std::cerr<<"bridge configuration invalid\n";return 2;}int fd=tcp_socket();if(fd<0)return 1;int yes=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_ANY);a.sin_port=htons(port);if(bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))||listen(fd,16)){perror("bridge");close(fd);return 1;}std::cout<<"OPAL wake bridge listening on "<<port<<"\n";auto worker=[&]{for(;;){int c=accept_cloexec(fd);if(c<0){if(errno==EINTR)continue;std::this_thread::sleep_for(std::chrono::milliseconds(10));continue;}bridge_peer(c,secret,mac);}};std::vector<std::thread>workers;for(int i=0;i<4;++i)workers.emplace_back(worker);for(auto&t:workers)t.join();return 0;}
static bool remote_wake(const std::string&address,uint16_t port,const std::string&secret){if(secret.empty())return false;auto colon=address.rfind(':');std::string host=colon==std::string::npos?address:address.substr(0,colon);if(colon!=std::string::npos){try{int parsed=std::stoi(address.substr(colon+1));if(parsed<1||parsed>65535)return false;port=static_cast<uint16_t>(parsed);}catch(...){return false;}}int fd=tcp_socket();if(fd<0)return false;socket_deadlines(fd);sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);if(inet_pton(AF_INET,host.c_str(),&a.sin_addr)!=1||!connect_deadline(fd,a,3000)){close(fd);return false;}char b[256]{};ssize_t n;do{n=recv(fd,b,sizeof(b)-1,0);}while(n<0&&errno==EINTR);if(n<=0){close(fd);return false;}auto nonce=trim(std::string(b,static_cast<size_t>(n)));auto proof=hmac_sha256_hex(secret,nonce)+"\n";if(!send_all(fd,proof)){close(fd);return false;}do{n=recv(fd,b,sizeof(b)-1,0);}while(n<0&&errno==EINTR);close(fd);return n>0&&std::string(b,static_cast<size_t>(n)).rfind("OK",0)==0;}
int wake_named(const std::string&name){Paths p=Paths::load();Ini h;if(!h.load(p.hosts)){std::cerr<<"no saved hosts\n";return 2;}auto mac=h.get(name,"mac"),bridge=h.get(name,"wake_bridge"),secret=h.get(name,"wake_secret");if(mac.empty()){std::cerr<<"host has no MAC configured\n";return 2;}bool ok=bridge.empty()?send_wol(mac):remote_wake(bridge,47992,secret);std::cout<<(ok?"wake request sent\n":"wake failed\n");return ok?0:1;}
}
