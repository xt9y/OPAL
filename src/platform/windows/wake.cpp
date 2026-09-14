#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>

#include <opal/config.hpp>
#include <opal/crypto.hpp>
#include <opal/tailnet.hpp>
#include <opal/wake.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace opal {
namespace {

bool winsock_ready()
{
    static std::once_flag once;
    static bool ready = false;
    std::call_once(once, [] { WSADATA data{}; ready = WSAStartup(MAKEWORD(2, 2), &data) == 0; });
    return ready;
}

SOCKET tcp_socket(){if(!winsock_ready())return INVALID_SOCKET;return WSASocketW(AF_INET,SOCK_STREAM,IPPROTO_TCP,nullptr,0,WSA_FLAG_OVERLAPPED);}
SOCKET udp_socket(){if(!winsock_ready())return INVALID_SOCKET;return WSASocketW(AF_INET,SOCK_DGRAM,IPPROTO_UDP,nullptr,0,WSA_FLAG_OVERLAPPED);}
void socket_deadlines(SOCKET socket,int seconds=3){const DWORD timeout=static_cast<DWORD>(std::max(0,seconds)*1000);(void)setsockopt(socket,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));(void)setsockopt(socket,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));}
bool connect_deadline(SOCKET socket,const sockaddr_in&address,int timeout_ms){u_long nonblocking=1;if(ioctlsocket(socket,FIONBIO,&nonblocking)==SOCKET_ERROR)return false;int rc=connect(socket,reinterpret_cast<const sockaddr*>(&address),sizeof(address));if(rc==0){nonblocking=0;(void)ioctlsocket(socket,FIONBIO,&nonblocking);return true;}if(WSAGetLastError()!=WSAEWOULDBLOCK){nonblocking=0;(void)ioctlsocket(socket,FIONBIO,&nonblocking);return false;}WSAPOLLFD descriptor{};descriptor.fd=socket;descriptor.events=POLLWRNORM;rc=WSAPoll(&descriptor,1,std::max(0,timeout_ms));int error=0;int length=sizeof(error);const bool ok=rc>0&&!(descriptor.revents&(POLLERR|POLLHUP|POLLNVAL))&&getsockopt(socket,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&length)==0&&error==0;nonblocking=0;(void)ioctlsocket(socket,FIONBIO,&nonblocking);return ok;}
bool send_all(SOCKET socket,const std::string&text){std::size_t offset=0;while(offset<text.size()){const int amount=static_cast<int>(std::min<std::size_t>(text.size()-offset,INT_MAX));const int written=send(socket,text.data()+offset,amount,0);if(written>0){offset+=static_cast<std::size_t>(written);continue;}if(written==SOCKET_ERROR&&WSAGetLastError()==WSAEINTR)continue;return false;}return true;}
void bridge_peer(SOCKET client,const std::string&secret,const std::string&mac){socket_deadlines(client);const auto nonce=random_hex(24);if(!send_all(client,nonce+"\n")){closesocket(client);return;}char buffer[512]{};int received=0;do{received=recv(client,buffer,static_cast<int>(sizeof(buffer)-1),0);}while(received==SOCKET_ERROR&&WSAGetLastError()==WSAEINTR);const std::string proof=trim(std::string(buffer,received>0?static_cast<std::size_t>(received):0));if(received>0&&secure_equal(proof,hmac_sha256_hex(secret,nonce))){const bool ok=send_wol(mac);(void)send_all(client,ok?"OK\n":"ERR\n");}else(void)send_all(client,"DENY\n");closesocket(client);}

bool remote_wake(const std::string&address,std::uint16_t port,const std::string&secret){if(secret.empty())return false;const auto colon=address.rfind(':');const std::string host=colon==std::string::npos?address:address.substr(0,colon);if(colon!=std::string::npos){try{const int parsed=std::stoi(address.substr(colon+1));if(parsed<1||parsed>65535)return false;port=static_cast<std::uint16_t>(parsed);}catch(...){return false;}}if(!is_tailnet_ipv4(host))return false;SOCKET socket=tcp_socket();if(socket==INVALID_SOCKET)return false;socket_deadlines(socket);sockaddr_in target{};target.sin_family=AF_INET;target.sin_port=htons(port);if(InetPtonA(AF_INET,host.c_str(),&target.sin_addr)!=1||!connect_deadline(socket,target,3000)){closesocket(socket);return false;}char buffer[256]{};int received=0;do{received=recv(socket,buffer,static_cast<int>(sizeof(buffer)-1),0);}while(received==SOCKET_ERROR&&WSAGetLastError()==WSAEINTR);if(received<=0){closesocket(socket);return false;}const auto nonce=trim(std::string(buffer,static_cast<std::size_t>(received)));const auto proof=hmac_sha256_hex(secret,nonce)+"\n";if(!send_all(socket,proof)){closesocket(socket);return false;}do{received=recv(socket,buffer,static_cast<int>(sizeof(buffer)-1),0);}while(received==SOCKET_ERROR&&WSAGetLastError()==WSAEINTR);closesocket(socket);return received>0&&std::string(buffer,static_cast<std::size_t>(received)).rfind("OK",0)==0;}

struct MagicStatus{bool known=false,supported=false,enabled=false;};
std::string utf8(const wchar_t*value){if(!value||!*value)return{};const int chars=static_cast<int>(wcslen(value));const int bytes=WideCharToMultiByte(CP_UTF8,0,value,chars,nullptr,0,nullptr,nullptr);if(bytes<=0)return{};std::string output(static_cast<std::size_t>(bytes),'\0');if(WideCharToMultiByte(CP_UTF8,0,value,chars,output.data(),bytes,nullptr,nullptr)!=bytes)return{};return output;}
std::string trim_copy(std::string value){while(!value.empty()&&(value.back()=='\r'||value.back()=='\n'||value.back()==' '||value.back()=='\t'))value.pop_back();std::size_t start=0;while(start<value.size()&&(value[start]==' '||value[start]=='\t'||value[start]=='\r'||value[start]=='\n'))++start;return value.substr(start);}
std::string ps_quote(const std::string&value){std::string output="'";for(char c:value){if(c=='\'')output+="''";else output+=c;}output+='\'';return output;}
std::string powershell_capture(const std::string&script){const std::string command="powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \""+script+"\" 2>NUL";FILE*pipe=_popen(command.c_str(),"r");if(!pipe)return{};std::string output;std::array<char,512>buffer{};while(fgets(buffer.data(),static_cast<int>(buffer.size()),pipe))output+=buffer.data();if(_pclose(pipe)!=0)return{};return trim_copy(output);}
bool powershell_run(const std::string&script){return std::system(("powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \""+script+"\" >NUL 2>NUL").c_str())==0;}
MagicStatus magic_status(const std::string&name){MagicStatus result;const auto value=powershell_capture("$p=Get-NetAdapterPowerManagement -Name "+ps_quote(name)+" -ErrorAction Stop; [Console]::Write($p.WakeOnMagicPacket.ToString())");if(value.empty())return result;result.known=true;if(value=="Unsupported")return result;result.supported=true;result.enabled=value=="Enabled";return result;}
bool enable_magic(const std::string&name){return powershell_run("Set-NetAdapterPowerManagement -Name "+ps_quote(name)+" -WakeOnMagicPacket Enabled -ErrorAction Stop");}
bool fast_startup_enabled(bool&known){known=false;HKEY key=nullptr;if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power",0,KEY_QUERY_VALUE,&key)!=ERROR_SUCCESS)return false;DWORD value=0,bytes=sizeof(value),type=0;const LSTATUS status=RegQueryValueExW(key,L"HiberbootEnabled",nullptr,&type,reinterpret_cast<BYTE*>(&value),&bytes);RegCloseKey(key);if(status!=ERROR_SUCCESS||type!=REG_DWORD)return false;known=true;return value!=0;}
bool disable_fast_startup(){HKEY key=nullptr;if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power",0,KEY_SET_VALUE,&key)!=ERROR_SUCCESS)return false;const DWORD value=0;const LSTATUS status=RegSetValueExW(key,L"HiberbootEnabled",0,REG_DWORD,reinterpret_cast<const BYTE*>(&value),sizeof(value));RegCloseKey(key);return status==ERROR_SUCCESS;}
std::string format_mac(const BYTE*address,ULONG length){if(!address||length!=6)return{};std::ostringstream output;output<<std::hex<<std::setfill('0');for(ULONG i=0;i<length;++i){if(i)output<<':';output<<std::setw(2)<<static_cast<unsigned>(address[i]);}return output.str();}
}

std::vector<std::uint8_t>wol_packet(const std::string&mac){std::array<unsigned,6>bytes{};char separator=0;std::istringstream input(mac);for(int i=0;i<6;++i){if(!(input>>std::hex>>bytes[static_cast<std::size_t>(i)])||bytes[static_cast<std::size_t>(i)]>0xff)return{};if(i<5&&(!(input>>separator)||separator!=':'))return{};}std::vector<std::uint8_t>packet(102,0xff);for(int repeat=0;repeat<16;++repeat)for(int i=0;i<6;++i)packet[6+repeat*6+i]=static_cast<std::uint8_t>(bytes[static_cast<std::size_t>(i)]);return packet;}
bool send_wol(const std::string&mac,const std::string&broadcast,std::uint16_t port){const auto packet=wol_packet(mac);if(packet.empty())return false;SOCKET socket=udp_socket();if(socket==INVALID_SOCKET)return false;int enabled=1;if(setsockopt(socket,SOL_SOCKET,SO_BROADCAST,reinterpret_cast<const char*>(&enabled),sizeof(enabled))==SOCKET_ERROR){closesocket(socket);return false;}sockaddr_in target{};target.sin_family=AF_INET;target.sin_port=htons(port);if(InetPtonA(AF_INET,broadcast.c_str(),&target.sin_addr)!=1){closesocket(socket);return false;}const int written=sendto(socket,reinterpret_cast<const char*>(packet.data()),static_cast<int>(packet.size()),0,reinterpret_cast<const sockaddr*>(&target),sizeof(target));closesocket(socket);return written==static_cast<int>(packet.size());}

int run_bridge(std::uint16_t port){const auto paths=Paths::load();(void)ensure_layout(paths);Ini config;if(!config.load(paths.root/"bridge.ini")){std::cerr<<"bridge not configured; run: opal bridge setup --mac XX:XX:XX:XX:XX:XX\n";return 2;}const auto secret=config.get("bridge","secret"),mac=config.get("bridge","mac");if(secret.size()<32||wol_packet(mac).empty()){std::cerr<<"bridge configuration invalid\n";return 2;}const auto bind_ip=local_tailnet_ipv4();if(!is_tailnet_ipv4(bind_ip)){std::cerr<<"wake bridge requires a connected Tailscale IPv4 address; refusing to listen publicly\n";return 2;}SOCKET listener=tcp_socket();if(listener==INVALID_SOCKET)return 1;int one=1;(void)setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,reinterpret_cast<const char*>(&one),sizeof(one));sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(port);if(InetPtonA(AF_INET,bind_ip.c_str(),&address.sin_addr)!=1||bind(listener,reinterpret_cast<const sockaddr*>(&address),sizeof(address))==SOCKET_ERROR||listen(listener,16)==SOCKET_ERROR){closesocket(listener);return 1;}std::cout<<"OPAL wake bridge listening on Tailscale "<<bind_ip<<":"<<port<<'\n';auto worker=[&]{for(;;){SOCKET client=accept(listener,nullptr,nullptr);if(client==INVALID_SOCKET){if(WSAGetLastError()==WSAEINTR)continue;std::this_thread::sleep_for(std::chrono::milliseconds(10));continue;}bridge_peer(client,secret,mac);}};std::vector<std::thread>workers;for(int i=0;i<4;++i)workers.emplace_back(worker);for(auto&worker_thread:workers)worker_thread.join();return 0;}

int wake_named(const std::string&name){const auto paths=Paths::load();Ini hosts;if(!hosts.load(paths.hosts)){std::cerr<<"no saved hosts\n";return 2;}const auto mac=hosts.get(name,"mac"),bridge=hosts.get(name,"wake_bridge"),secret=hosts.get(name,"wake_secret");if(mac.empty()){std::cerr<<"host has no MAC configured\n";return 2;}const bool ok=bridge.empty()?send_wol(mac):remote_wake(bridge,47992,secret);if(ok&&bridge.empty())std::cout<<"local-LAN wake request sent (remote wake needs an OPAL/Tailscale relay on the host LAN)\n";else std::cout<<(ok?"wake request sent\n":"wake failed\n");return ok?0:1;}

WakeCapabilityReport configure_host_wake(){WakeCapabilityReport report;bool fast_known=false;bool fast_enabled=fast_startup_enabled(fast_known);if(fast_known&&fast_enabled){if(disable_fast_startup())fast_enabled=false;else report.notes.push_back("Windows Fast Startup is enabled and OPAL could not disable it. Run OPAL once as Administrator for the most reliable shutdown wake behavior.");}ULONG size=16384;std::vector<unsigned char>storage(size);auto*addresses=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());ULONG result=GetAdaptersAddresses(AF_UNSPEC,GAA_FLAG_INCLUDE_PREFIX,nullptr,addresses,&size);if(result==ERROR_BUFFER_OVERFLOW){storage.resize(size);addresses=reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());result=GetAdaptersAddresses(AF_UNSPEC,GAA_FLAG_INCLUDE_PREFIX,nullptr,addresses,&size);}if(result!=NO_ERROR){report.notes.push_back("Windows could not enumerate physical network adapters.");return report;}for(auto*current=addresses;current;current=current->Next){WakeLink link=WakeLink::Other;if(current->IfType==IF_TYPE_ETHERNET_CSMACD)link=WakeLink::Ethernet;else if(current->IfType==IF_TYPE_IEEE80211)link=WakeLink::Wifi;else continue;const auto mac=format_mac(current->PhysicalAddress,current->PhysicalAddressLength);if(mac.empty())continue;WakeAdapterCapability adapter;adapter.name=utf8(current->FriendlyName);if(adapter.name.empty()&&current->AdapterName)adapter.name=current->AdapterName;adapter.description=utf8(current->Description);adapter.mac=mac;adapter.link=link;adapter.connected=current->OperStatus==IfOperStatusUp;auto magic=magic_status(adapter.name);if(magic.known&&magic.supported&&!magic.enabled){(void)enable_magic(adapter.name);magic=magic_status(adapter.name);}adapter.magic_packet_known=magic.known;adapter.magic_packet=magic.supported;adapter.configured=magic.enabled;adapter.persistent=magic.enabled;if(!magic.known){adapter.sleep=adapter.hibernate=adapter.shutdown=WakePowerSupport::Unknown;adapter.limitation="Windows did not expose WakeOnMagicPacket for this adapter. The driver may use a vendor-specific property; update the NIC driver or enable Magic Packet wake in Device Manager.";}else if(!magic.supported){adapter.sleep=adapter.hibernate=adapter.shutdown=WakePowerSupport::No;adapter.limitation="The Windows network driver reports Wake on Magic Packet as unsupported.";}else if(link==WakeLink::Wifi){adapter.sleep=magic.enabled?WakePowerSupport::Yes:WakePowerSupport::Unknown;adapter.hibernate=WakePowerSupport::Unknown;adapter.shutdown=WakePowerSupport::No;adapter.limitation="Wi-Fi uses WoWLAN. Sleep wake can work when the chipset/firmware supports it; OPAL does not promise Wi-Fi wake from shutdown.";}else{adapter.sleep=magic.enabled?WakePowerSupport::Yes:WakePowerSupport::Unknown;adapter.hibernate=WakePowerSupport::Unknown;adapter.shutdown=WakePowerSupport::Unknown;adapter.limitation="Hibernate/shutdown wake also depends on motherboard standby power and firmware PCIe/PME settings. Disable ErP if the NIC loses power while the PC is off.";if(fast_known&&fast_enabled)adapter.limitation+=" Windows Fast Startup is still enabled, so shutdown wake may fail.";}if(magic.supported&&!magic.enabled)adapter.limitation+=" OPAL could not enable the driver setting; run OPAL once as Administrator or enable Wake on Magic Packet in the adapter properties.";report.adapters.push_back(std::move(adapter));}report.preferred=preferred_wake_adapter(report.adapters);report.notes.push_back("OPAL never opens a public Wake-on-LAN port. Cross-network wake requires an always-on OPAL/Tailscale relay on the sleeping host's LAN.");report.notes.push_back("Tailscale cannot run while the target is off; it carries the authenticated request to the relay, which sends the local Magic Packet.");report.notes.push_back("Wake is impossible with the PSU off, AC removed, or firmware configured to remove NIC standby power.");return report;}

}
