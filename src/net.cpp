#include <opal/net.hpp>
#include <opal/config.hpp>
#include <opal/crypto.hpp>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <ifaddrs.h>
#include <iostream>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace opal {
namespace {
struct TlsLineState { std::string pending; };

int tls_line_state_index(){
    static const int index=SSL_get_ex_new_index(0,nullptr,nullptr,nullptr,nullptr);
    return index;
}
TlsLineState *tls_line_state(SSL *ssl){
    if(!ssl)return nullptr;
    const int index=tls_line_state_index();if(index<0)return nullptr;
    auto *state=static_cast<TlsLineState*>(SSL_get_ex_data(ssl,index));
    if(!state){state=new TlsLineState;if(SSL_set_ex_data(ssl,index,state)!=1){delete state;return nullptr;}}
    return state;
}
void clear_tls_line_state(SSL *ssl){
    if(!ssl)return;const int index=tls_line_state_index();if(index<0)return;
    auto *state=static_cast<TlsLineState*>(SSL_get_ex_data(ssl,index));delete state;SSL_set_ex_data(ssl,index,nullptr);
}
bool take_pending_line(TlsLineState &state,std::string &line,size_t limit){
    const auto newline=state.pending.find('\n');
    if(newline==std::string::npos)return false;
    if(newline>limit){state.pending.clear();return false;}
    line.assign(state.pending.data(),newline);state.pending.erase(0,newline+1);
    line.erase(std::remove(line.begin(),line.end(),'\r'),line.end());return true;
}
int set_cloexec(int fd){if(fd<0)return -1;int f=fcntl(fd,F_GETFD,0);return f<0||fcntl(fd,F_SETFD,f|FD_CLOEXEC)<0?-1:0;}
int make_socket(int family,int type,int protocol){
#ifdef SOCK_CLOEXEC
    int cloexec_fd=socket(family,type|SOCK_CLOEXEC,protocol);
    if(cloexec_fd>=0)return cloexec_fd;
    if(errno!=EINVAL&&errno!=EPROTONOSUPPORT)return -1;
#endif
    int fd=socket(family,type,protocol);if(fd>=0&&set_cloexec(fd)!=0){close(fd);return -1;}return fd;
}
int accept_socket(int lfd){
#ifdef __linux__
    int cloexec_fd=accept4(lfd,nullptr,nullptr,SOCK_CLOEXEC);
    if(cloexec_fd>=0)return cloexec_fd;
    if(errno!=ENOSYS&&errno!=EINVAL)return -1;
#endif
    int fd=accept(lfd,nullptr,nullptr);if(fd>=0&&set_cloexec(fd)!=0){close(fd);return -1;}return fd;
}
bool wait_fd(int fd,short events,std::chrono::steady_clock::time_point deadline){
    for(;;){auto now=std::chrono::steady_clock::now();if(now>=deadline)return false;auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count();pollfd p{fd,events,0};int rc=poll(&p,1,static_cast<int>(std::max<long long>(1,ms)));if(rc<0&&errno==EINTR)continue;if(rc<=0)return false;if(p.revents&(POLLERR|POLLHUP|POLLNVAL))return false;if(p.revents&events)return true;}
}
bool ssl_handshake(SSL*s,bool server,int timeout_ms){
    int fd=SSL_get_fd(s);if(fd<0)return false;int flags=fcntl(fd,F_GETFL,0);if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0)return false;
    auto restore=[&]{fcntl(fd,F_SETFL,flags);};auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(std::max(1,timeout_ms));
    for(;;){int rc=server?SSL_accept(s):SSL_connect(s);if(rc==1){restore();return true;}int e=SSL_get_error(s,rc);if(e!=SSL_ERROR_WANT_READ&&e!=SSL_ERROR_WANT_WRITE){restore();return false;}if(!wait_fd(fd,e==SSL_ERROR_WANT_READ?POLLIN:POLLOUT,deadline)){restore();return false;}}
}
std::string fingerprint_x509(X509*c){if(!c)return{};unsigned char md[EVP_MAX_MD_SIZE];unsigned int n=0;return X509_digest(c,EVP_sha256(),md,&n)==1?hex(md,n):std::string();}
}

int listen_tcp(uint16_t port,const std::string&bind_address){addrinfo hints{},*res=nullptr;hints.ai_family=AF_UNSPEC;hints.ai_socktype=SOCK_STREAM;hints.ai_flags=AI_PASSIVE;std::string ps=std::to_string(port);const char*host=(bind_address=="0.0.0.0"||bind_address.empty())?nullptr:bind_address.c_str();if(getaddrinfo(host,ps.c_str(),&hints,&res)!=0)return -1;int fd=-1;for(auto*p=res;p;p=p->ai_next){fd=make_socket(p->ai_family,p->ai_socktype,p->ai_protocol);if(fd<0)continue;int yes=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));if(bind(fd,p->ai_addr,p->ai_addrlen)==0&&listen(fd,32)==0)break;close(fd);fd=-1;}freeaddrinfo(res);return fd;}
bool set_tcp_nodelay(int fd){int yes=1;return fd>=0&&setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof(yes))==0;}
int connect_tcp(const std::string&host,uint16_t port){addrinfo hints{},*res=nullptr;hints.ai_family=AF_UNSPEC;hints.ai_socktype=SOCK_STREAM;auto ps=std::to_string(port);if(getaddrinfo(host.c_str(),ps.c_str(),&hints,&res)!=0)return -1;int fd=-1;for(auto*p=res;p;p=p->ai_next){fd=make_socket(p->ai_family,p->ai_socktype,p->ai_protocol);if(fd<0)continue;int flags=fcntl(fd,F_GETFL,0);if(flags<0){close(fd);fd=-1;continue;}fcntl(fd,F_SETFL,flags|O_NONBLOCK);int rc=connect(fd,p->ai_addr,p->ai_addrlen);bool ok=rc==0;if(!ok&&errno==EINPROGRESS){auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);if(wait_fd(fd,POLLOUT,deadline)){int err=0;socklen_t len=sizeof(err);ok=getsockopt(fd,SOL_SOCKET,SO_ERROR,&err,&len)==0&&err==0;}}fcntl(fd,F_SETFL,flags);if(ok){set_tcp_nodelay(fd);break;}close(fd);fd=-1;}freeaddrinfo(res);return fd;}
bool ensure_tls_certificate(const std::string&cert,const std::string&key){if(std::filesystem::exists(cert)&&std::filesystem::exists(key))return chmod(key.c_str(),0600)==0;auto cmd="openssl req -x509 -newkey ed25519 -nodes -keyout "+shell_quote(key)+" -out "+shell_quote(cert)+" -days 3650 -subj /CN=OPAL >/dev/null 2>&1";if(std::system(cmd.c_str())!=0)return false;return chmod(key.c_str(),0600)==0;}
SSL_CTX*server_tls_context(const std::string&cert,const std::string&key){SSL_CTX*c=SSL_CTX_new(TLS_server_method());if(!c)return nullptr;SSL_CTX_set_min_proto_version(c,TLS1_3_VERSION);if(SSL_CTX_use_certificate_file(c,cert.c_str(),SSL_FILETYPE_PEM)!=1||SSL_CTX_use_PrivateKey_file(c,key.c_str(),SSL_FILETYPE_PEM)!=1){SSL_CTX_free(c);return nullptr;}return c;}
SSL_CTX*client_tls_context(){SSL_CTX*c=SSL_CTX_new(TLS_client_method());if(!c)return nullptr;SSL_CTX_set_min_proto_version(c,TLS1_3_VERSION);SSL_CTX_set_verify(c,SSL_VERIFY_NONE,nullptr);return c;}
TlsConn accept_tls_timeout(SSL_CTX*ctx,int lfd,int timeout_ms){TlsConn c;c.fd=accept_socket(lfd);if(c.fd<0)return c;set_tcp_nodelay(c.fd);c.ssl=SSL_new(ctx);if(!c.ssl){close_tls(c);return c;}SSL_set_fd(c.ssl,c.fd);if(!ssl_handshake(c.ssl,true,timeout_ms))close_tls(c);return c;}
TlsConn accept_tls(SSL_CTX*ctx,int lfd){return accept_tls_timeout(ctx,lfd,30000);}
TlsConn connect_tls(SSL_CTX*ctx,const std::string&host,uint16_t port){TlsConn c;c.fd=connect_tcp(host,port);if(c.fd<0)return c;c.ssl=SSL_new(ctx);if(!c.ssl){close_tls(c);return c;}SSL_set_fd(c.ssl,c.fd);SSL_set_tlsext_host_name(c.ssl,host.c_str());if(!ssl_handshake(c.ssl,false,5000))close_tls(c);return c;}
TlsConn connect_tls_retry(SSL_CTX*ctx,const std::string&host,uint16_t port,int timeout_ms,int retry_ms){timeout_ms=std::max(0,timeout_ms);retry_ms=std::max(1,retry_ms);auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout_ms);for(;;){auto c=connect_tls(ctx,host,port);if(c.ssl)return c;auto now=std::chrono::steady_clock::now();if(now>=deadline)return{};auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now);std::this_thread::sleep_for(std::min(std::chrono::milliseconds(retry_ms),remaining));}}
void close_tls(TlsConn&c){if(c.ssl){clear_tls_line_state(c.ssl);SSL_set_quiet_shutdown(c.ssl,1);SSL_free(c.ssl);c.ssl=nullptr;}if(c.fd>=0){close(c.fd);c.fd=-1;}}
bool tls_write_all_timeout(SSL*s,const void*d,size_t n,int timeout_ms){if(!s)return false;int fd=SSL_get_fd(s);if(fd<0)return false;int flags=fcntl(fd,F_GETFL,0);if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0)return false;auto restore=[&]{fcntl(fd,F_SETFL,flags);};auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(std::max(1,timeout_ms));auto*p=static_cast<const unsigned char*>(d);while(n){int w=SSL_write(s,p,static_cast<int>(std::min<size_t>(n,1<<20)));if(w>0){p+=w;n-=static_cast<size_t>(w);continue;}int e=SSL_get_error(s,w);if(e!=SSL_ERROR_WANT_WRITE&&e!=SSL_ERROR_WANT_READ){restore();return false;}if(!wait_fd(fd,e==SSL_ERROR_WANT_READ?POLLIN:POLLOUT,deadline)){restore();return false;}}restore();return true;}
bool tls_write_all(SSL*s,const void*d,size_t n){return tls_write_all_timeout(s,d,n,5000);}
bool tls_write_line_timeout(SSL*s,const std::string&line,int timeout_ms){std::string x=line;if(x.empty()||x.back()!='\n')x+='\n';return tls_write_all_timeout(s,x.data(),x.size(),timeout_ms);}
bool tls_write_line(SSL*s,const std::string&line){return tls_write_line_timeout(s,line,5000);}
bool tls_read_line_timeout(SSL*s,std::string&line,int timeout_ms,size_t limit){
    line.clear();if(!s||limit==0)return false;auto *state=tls_line_state(s);if(!state)return false;
    if(take_pending_line(*state,line,limit))return true;
    if(state->pending.size()>=limit){state->pending.clear();return false;}
    int fd=SSL_get_fd(s);if(fd<0)return false;int flags=fcntl(fd,F_GETFL,0);if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0)return false;
    auto restore=[&]{fcntl(fd,F_SETFL,flags);};auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(std::max(1,timeout_ms));char buf[2048];
    for(;;){
        const size_t room=limit-state->pending.size();if(room==0){state->pending.clear();restore();return false;}
        int n=SSL_read(s,buf,static_cast<int>(std::min(room,sizeof(buf))));
        if(n>0){state->pending.append(buf,static_cast<size_t>(n));if(take_pending_line(*state,line,limit)){restore();return true;}continue;}
        int e=SSL_get_error(s,n);if(e!=SSL_ERROR_WANT_READ&&e!=SSL_ERROR_WANT_WRITE){restore();return false;}
        if(!wait_fd(fd,e==SSL_ERROR_WANT_WRITE?POLLOUT:POLLIN,deadline)){restore();return false;}
    }
}
bool tls_read_line(SSL*s,std::string&line,size_t limit){return tls_read_line_timeout(s,line,30000,limit);}
bool tls_line_ready(SSL*s){
    if(!s)return false;const int index=tls_line_state_index();if(index<0)return false;
    auto *state=static_cast<TlsLineState*>(SSL_get_ex_data(s,index));
    return state&&state->pending.find('\n')!=std::string::npos;
}
std::string peer_fingerprint(SSL*s){X509*c=SSL_get1_peer_certificate(s);if(!c)return{};auto out=fingerprint_x509(c);X509_free(c);return out;}
std::string local_fingerprint(SSL*s){return s?fingerprint_x509(SSL_get_certificate(s)):std::string();}
std::string primary_ipv4(){ifaddrs*list=nullptr;if(getifaddrs(&list)!=0)return"127.0.0.1";std::string out="127.0.0.1";for(auto*p=list;p;p=p->ifa_next){if(!p->ifa_addr||p->ifa_addr->sa_family!=AF_INET)continue;auto*a=reinterpret_cast<sockaddr_in*>(p->ifa_addr);char buf[INET_ADDRSTRLEN]{};if(!inet_ntop(AF_INET,&a->sin_addr,buf,sizeof(buf)))continue;std::string ip=buf;if(ip.rfind("127.",0)!=0){out=ip;break;}}freeifaddrs(list);return out;}
}
