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
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace opal {
int listen_tcp(uint16_t port,const std::string&bind_address){addrinfo hints{},*res=nullptr;hints.ai_family=AF_UNSPEC;hints.ai_socktype=SOCK_STREAM;hints.ai_flags=AI_PASSIVE;std::string ps=std::to_string(port);const char*host=(bind_address=="0.0.0.0"||bind_address.empty())?nullptr:bind_address.c_str();if(getaddrinfo(host,ps.c_str(),&hints,&res)!=0)return -1;int fd=-1;for(auto*p=res;p;p=p->ai_next){fd=socket(p->ai_family,p->ai_socktype,p->ai_protocol);if(fd<0)continue;int yes=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));if(bind(fd,p->ai_addr,p->ai_addrlen)==0&&listen(fd,32)==0)break;close(fd);fd=-1;}freeaddrinfo(res);return fd;}
int connect_tcp(const std::string&host,uint16_t port){addrinfo hints{},*res=nullptr;hints.ai_family=AF_UNSPEC;hints.ai_socktype=SOCK_STREAM;auto ps=std::to_string(port);if(getaddrinfo(host.c_str(),ps.c_str(),&hints,&res)!=0)return -1;int fd=-1;for(auto*p=res;p;p=p->ai_next){fd=socket(p->ai_family,p->ai_socktype,p->ai_protocol);if(fd<0)continue;if(connect(fd,p->ai_addr,p->ai_addrlen)==0)break;close(fd);fd=-1;}freeaddrinfo(res);return fd;}
bool ensure_tls_certificate(const std::string&cert,const std::string&key){if(std::filesystem::exists(cert)&&std::filesystem::exists(key))return true;auto cmd="openssl req -x509 -newkey ed25519 -nodes -keyout "+shell_quote(key)+" -out "+shell_quote(cert)+" -days 3650 -subj /CN=OPAL >/dev/null 2>&1";return std::system(cmd.c_str())==0;}
SSL_CTX*server_tls_context(const std::string&cert,const std::string&key){SSL_CTX*c=SSL_CTX_new(TLS_server_method());if(!c)return nullptr;SSL_CTX_set_min_proto_version(c,TLS1_3_VERSION);if(SSL_CTX_use_certificate_file(c,cert.c_str(),SSL_FILETYPE_PEM)!=1||SSL_CTX_use_PrivateKey_file(c,key.c_str(),SSL_FILETYPE_PEM)!=1){SSL_CTX_free(c);return nullptr;}return c;}
SSL_CTX*client_tls_context(){SSL_CTX*c=SSL_CTX_new(TLS_client_method());if(!c)return nullptr;SSL_CTX_set_min_proto_version(c,TLS1_3_VERSION);SSL_CTX_set_verify(c,SSL_VERIFY_NONE,nullptr);return c;}
TlsConn accept_tls(SSL_CTX*ctx,int lfd){TlsConn c;c.fd=accept(lfd,nullptr,nullptr);if(c.fd<0)return c;c.ssl=SSL_new(ctx);SSL_set_fd(c.ssl,c.fd);if(SSL_accept(c.ssl)!=1){close_tls(c);}return c;}
TlsConn connect_tls(SSL_CTX*ctx,const std::string&host,uint16_t port){TlsConn c;c.fd=connect_tcp(host,port);if(c.fd<0)return c;c.ssl=SSL_new(ctx);SSL_set_fd(c.ssl,c.fd);SSL_set_tlsext_host_name(c.ssl,host.c_str());if(SSL_connect(c.ssl)!=1)close_tls(c);return c;}
TlsConn connect_tls_retry(SSL_CTX*ctx,const std::string&host,uint16_t port,int timeout_ms,int retry_ms){timeout_ms=std::max(0,timeout_ms);retry_ms=std::max(1,retry_ms);auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout_ms);for(;;){auto c=connect_tls(ctx,host,port);if(c.ssl)return c;auto now=std::chrono::steady_clock::now();if(now>=deadline)return{};auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now);std::this_thread::sleep_for(std::min(std::chrono::milliseconds(retry_ms),remaining));}}
void close_tls(TlsConn&c){if(c.ssl){SSL_shutdown(c.ssl);SSL_free(c.ssl);c.ssl=nullptr;}if(c.fd>=0){close(c.fd);c.fd=-1;}}
bool tls_write_all(SSL*s,const void*d,size_t n){auto*p=static_cast<const unsigned char*>(d);while(n){int w=SSL_write(s,p,static_cast<int>(std::min<size_t>(n,1<<20)));if(w<=0)return false;p+=w;n-=static_cast<size_t>(w);}return true;}

static bool wait_for_ssl_fd(int fd,short events,std::chrono::steady_clock::time_point deadline){
    for(;;){
        auto now=std::chrono::steady_clock::now();
        if(now>=deadline)return false;
        auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count();
        pollfd pfd{fd,events,0};
        int rc=poll(&pfd,1,static_cast<int>(std::max<long long>(1,remaining)));
        if(rc<0&&errno==EINTR)continue;
        if(rc<=0)return false;
        if(pfd.revents&(POLLERR|POLLHUP|POLLNVAL))return false;
        if(pfd.revents&events)return true;
    }
}

bool tls_write_all_timeout(SSL*s,const void*d,size_t n,int timeout_ms){
    if(!s)return false;
    int fd=SSL_get_fd(s);
    if(fd<0)return false;
    int flags=fcntl(fd,F_GETFL,0);
    if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0)return false;
    auto restore=[&]{fcntl(fd,F_SETFL,flags);};
    auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(std::max(0,timeout_ms));
    auto*p=static_cast<const unsigned char*>(d);
    while(n){
        int w=SSL_write(s,p,static_cast<int>(std::min<size_t>(n,1<<20)));
        if(w>0){p+=w;n-=static_cast<size_t>(w);continue;}
        int err=SSL_get_error(s,w);
        if(err!=SSL_ERROR_WANT_WRITE&&err!=SSL_ERROR_WANT_READ){restore();return false;}
        short events=err==SSL_ERROR_WANT_READ?POLLIN:POLLOUT;
        if(!wait_for_ssl_fd(fd,events,deadline)){restore();return false;}
    }
    restore();
    return true;
}

bool tls_write_line(SSL*s,const std::string&line){std::string x=line;if(x.empty()||x.back()!='\n')x+='\n';return tls_write_all(s,x.data(),x.size());}
bool tls_read_line(SSL*s,std::string&line,size_t limit){line.clear();char ch;while(line.size()<limit){int n=SSL_read(s,&ch,1);if(n<=0)return false;if(ch=='\n')return true;if(ch!='\r')line+=ch;}return false;}
std::string peer_fingerprint(SSL*s){X509*c=SSL_get1_peer_certificate(s);if(!c)return{};unsigned char md[EVP_MAX_MD_SIZE];unsigned int n=0;std::string out;if(X509_digest(c,EVP_sha256(),md,&n)==1)out=hex(md,n);X509_free(c);return out;}
std::string primary_ipv4(){ifaddrs*list=nullptr;if(getifaddrs(&list)!=0)return"127.0.0.1";std::string out="127.0.0.1";for(auto*p=list;p;p=p->ifa_next){if(!p->ifa_addr||p->ifa_addr->sa_family!=AF_INET)continue;auto*a=reinterpret_cast<sockaddr_in*>(p->ifa_addr);char buf[INET_ADDRSTRLEN]{};if(!inet_ntop(AF_INET,&a->sin_addr,buf,sizeof(buf)))continue;std::string ip=buf;if(ip.rfind("127.",0)!=0){out=ip;break;}}freeifaddrs(list);return out;}
}
