#include <opal/wake.hpp>
#include <opal/config.hpp>
#include <opal/crypto.hpp>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <cstring>
#include <iostream>
#include <sstream>

namespace opal {
std::vector<uint8_t>wol_packet(const std::string&mac){std::array<unsigned,6>b{};char c;std::istringstream ss(mac);for(int i=0;i<6;i++){if(!(ss>>std::hex>>b[i]))return{};if(i<5&&!(ss>>c))return{};}std::vector<uint8_t>p(102,0xff);for(int r=0;r<16;r++)for(int i=0;i<6;i++)p[6+r*6+i]=static_cast<uint8_t>(b[i]);return p;}
bool send_wol(const std::string&mac,const std::string&broadcast,uint16_t port){auto p=wol_packet(mac);if(p.empty())return false;int fd=socket(AF_INET,SOCK_DGRAM,0);if(fd<0)return false;int yes=1;setsockopt(fd,SOL_SOCKET,SO_BROADCAST,&yes,sizeof(yes));sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);if(inet_pton(AF_INET,broadcast.c_str(),&a.sin_addr)!=1){close(fd);return false;}auto n=sendto(fd,p.data(),p.size(),0,reinterpret_cast<sockaddr*>(&a),sizeof(a));close(fd);return n==static_cast<ssize_t>(p.size());}
// Bridge intentionally uses an authenticated request over a tiny TCP socket; put it behind zrok/private networking for Internet use.
int run_bridge(uint16_t port){Paths p=Paths::load();ensure_layout(p);Ini cfg;if(!cfg.load(p.root/"bridge.ini")){std::cerr<<"bridge not configured; run: opal bridge setup --mac XX:XX:XX:XX:XX:XX\n";return 2;}auto secret=cfg.get("bridge","secret"),mac=cfg.get("bridge","mac");int fd=socket(AF_INET,SOCK_STREAM,0);int yes=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_ANY);a.sin_port=htons(port);if(bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))||listen(fd,16)){perror("bridge");return 1;}std::cout<<"OPAL wake bridge listening on "<<port<<"\n";for(;;){int c=accept(fd,nullptr,nullptr);if(c<0)continue;auto nonce=random_hex(24);send(c,nonce.data(),nonce.size(),0);send(c,"\n",1,0);char buf[512]{};auto n=recv(c,buf,sizeof(buf)-1,0);std::string got(buf,n>0?n:0);got=trim(got);if(secure_equal(got,hmac_sha256_hex(secret,nonce))){bool ok=send_wol(mac);send(c,ok?"OK\n":"ERR\n",ok?3:4,0);}else send(c,"DENY\n",5,0);close(c);} }
static bool remote_wake(const std::string&address,uint16_t port,const std::string&secret){auto colon=address.find(':');std::string host=colon==std::string::npos?address:address.substr(0,colon);if(colon!=std::string::npos)port=static_cast<uint16_t>(std::stoi(address.substr(colon+1)));int fd=socket(AF_INET,SOCK_STREAM,0);sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);if(inet_pton(AF_INET,host.c_str(),&a.sin_addr)!=1){close(fd);return false;}if(connect(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))){close(fd);return false;}char b[256]{};auto n=recv(fd,b,sizeof(b)-1,0);if(n<=0){close(fd);return false;}auto nonce=trim(std::string(b,n));auto proof=hmac_sha256_hex(secret,nonce)+"\n";send(fd,proof.data(),proof.size(),0);n=recv(fd,b,sizeof(b)-1,0);close(fd);return n>0&&std::string(b,n).rfind("OK",0)==0;}
int wake_named(const std::string&name){Paths p=Paths::load();Ini h;if(!h.load(p.hosts)){std::cerr<<"no saved hosts\n";return 2;}auto mac=h.get(name,"mac"),bridge=h.get(name,"wake_bridge"),secret=h.get(name,"wake_secret");if(mac.empty()){std::cerr<<"host has no MAC configured\n";return 2;}bool ok=bridge.empty()?send_wol(mac):remote_wake(bridge,47992,secret);std::cout<<(ok?"wake request sent\n":"wake failed\n");return ok?0:1;}
}
