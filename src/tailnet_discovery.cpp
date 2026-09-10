#include <opal/tailnet_discovery.hpp>
#include <opal/connection_code.hpp>
#include <opal/crypto.hpp>
#include <opal/udp_socket_ops.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <sstream>
#include <string_view>

namespace opal {
namespace {
using Clock=std::chrono::steady_clock;
constexpr std::size_t kDiscoveryMessageBytes=768;

bool valid_public_key(const std::string&value){return unhex(value).size()==32;}
bool valid_id(const std::string&id){std::string checked;return parse_connection_code(format_connection_code(id),checked)&&checked==id;}

std::string offer_transcript(const std::string&id,const std::string&session_id,
                             const std::string&client_public_key,const std::string&client_nonce,
                             const std::string&host_public_key,const std::string&host_nonce,
                             std::uint16_t peer_port){
    return "OPAL-TAILNET-OFFER-v1\n"+id+"\n"+session_id+"\n"+client_public_key+"\n"+
           client_nonce+"\n"+host_public_key+"\n"+host_nonce+"\n"+std::to_string(peer_port);
}

bool candidate_from_udp(const UdpEndpoint&source,UdpCandidate&endpoint){std::string host;std::uint16_t port=0;if(!udp_endpoint_numeric(source,host,port))return false;endpoint={host,port};return true;}

bool parse_discover(std::string_view wire,std::string&id,std::string&client_public_key,
                    std::string&client_nonce,std::uint16_t&peer_port){
    std::istringstream in{std::string(wire)};std::string word,port_text,extra;
    if(!(in>>word>>id>>client_public_key>>client_nonce>>port_text)||in>>extra||word!="OPAL_TAILNET_DISCOVER_V1")return false;
    try{size_t used=0;const unsigned long port=std::stoul(port_text,&used);if(used!=port_text.size()||port<1||port>65535)return false;peer_port=static_cast<std::uint16_t>(port);}catch(...){return false;}
    return valid_id(id)&&valid_public_key(client_public_key)&&unhex(client_nonce).size()==16;
}

bool parse_offer(std::string_view wire,std::string&id,std::string&session_id,
                 std::string&host_public_key,std::string&host_nonce,
                 std::uint16_t&peer_port,std::string&signature){
    std::istringstream in{std::string(wire)};std::string word,port_text,extra;
    if(!(in>>word>>id>>session_id>>host_public_key>>host_nonce>>port_text>>signature)||in>>extra||word!="OPAL_TAILNET_OFFER_V1")return false;
    try{size_t used=0;const unsigned long port=std::stoul(port_text,&used);if(used!=port_text.size()||port<1||port>65535)return false;peer_port=static_cast<std::uint16_t>(port);}catch(...){return false;}
    return valid_id(id)&&unhex(session_id).size()==16&&valid_public_key(host_public_key)&&unhex(host_nonce).size()==16&&unhex(signature).size()==64;
}

int remaining_ms(Clock::time_point deadline){const auto now=Clock::now();if(now>=deadline)return 0;return std::max(1,static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count()));}
}

UdpSocket open_tailnet_discovery_listener(std::uint16_t port,std::string bind_host,std::string&error){return open_udp_listener(port,bind_host,error);}

bool wait_tailnet_client(UdpSocket&listener,const std::string&host_public_key,
                         const std::filesystem::path&host_private_key,
                         TailnetHostResult&result,int timeout_ms,std::string&error){
    result={};error.clear();const auto id=connection_id_from_public_key(host_public_key);
    if(!listener.valid()||listener.local_port==0||id.empty()||!valid_public_key(host_public_key)){error="Tailscale discovery listener unavailable";return false;}
    const auto deadline=Clock::now()+std::chrono::milliseconds(std::max(1,timeout_ms));std::array<std::uint8_t,kDiscoveryMessageBytes>buffer{};
    while(remaining_ms(deadline)>0){
        UdpEndpoint source{};const int n=recv_datagram(listener,buffer,source,remaining_ms(deadline));
        if(n==-2)continue;if(n<0){error="Tailscale discovery receive failed";return false;}if(n<=0||n>static_cast<int>(buffer.size()))continue;
        std::string request_id,client_public_key,client_nonce;std::uint16_t client_peer_port=0;
        if(!parse_discover(std::string_view(reinterpret_cast<const char*>(buffer.data()),static_cast<std::size_t>(n)),request_id,client_public_key,client_nonce,client_peer_port)||request_id!=id)continue;
        UdpCandidate client;if(!candidate_from_udp(source,client)||client.port!=client_peer_port)continue;
        UdpSocket peer_socket=duplicate_udp_socket(listener);if(!peer_socket.valid()){error="Tailscale peer socket duplication failed";return false;}
        const auto session_id=random_hex(16),host_nonce=random_hex(16);const auto transcript=offer_transcript(id,session_id,client_public_key,client_nonce,host_public_key,host_nonce,listener.local_port);const auto signature=sign_hex(host_private_key,transcript);
        if(signature.empty()){close_udp_socket(peer_socket);error="Tailscale discovery identity signing failed";return false;}
        const std::string offer="OPAL_TAILNET_OFFER_V1 "+id+" "+session_id+" "+host_public_key+" "+host_nonce+" "+std::to_string(listener.local_port)+" "+signature;
        if(!send_datagram(listener,source,std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(offer.data()),offer.size()))){close_udp_socket(peer_socket);continue;}
        result.socket=peer_socket;result.client=client;result.connection_id=id;result.session_id=session_id;result.client_public_key=client_public_key;result.client_nonce=client_nonce;result.host_nonce=host_nonce;return true;
    }
    error="Tailscale discovery timeout";return false;
}

bool discover_tailnet_host(const std::string&connection_id,const std::string&client_public_key,
                           const std::string&destination_host,TailnetClientResult&result,
                           std::string&error,int timeout_ms,std::uint16_t destination_port){
    result={};error.clear();if(!valid_id(connection_id)||!valid_public_key(client_public_key)||destination_host.empty()){error="invalid Tailscale discovery identity";return false;}
    auto peer_socket=open_udp_socket();if(!peer_socket.valid()){error="Tailscale discovery socket failed";return false;}
    UdpEndpoint target{};if(!resolve_udp_endpoint(destination_host,destination_port,target)){close_udp_socket(peer_socket);error="Tailscale destination unavailable";return false;}
    const auto client_nonce=random_hex(16);const std::string request="OPAL_TAILNET_DISCOVER_V1 "+connection_id+" "+client_public_key+" "+client_nonce+" "+std::to_string(peer_socket.local_port);
    const auto deadline=Clock::now()+std::chrono::milliseconds(std::max(1,timeout_ms));auto next_send=Clock::time_point{};std::array<std::uint8_t,kDiscoveryMessageBytes>buffer{};std::string rejection_error;
    while(remaining_ms(deadline)>0){
        const auto now=Clock::now();if(next_send.time_since_epoch().count()==0||now>=next_send){(void)send_datagram(peer_socket,target,std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(request.data()),request.size()));next_send=now+std::chrono::milliseconds(75);}
        UdpEndpoint source{};const int n=recv_datagram(peer_socket,buffer,source,std::min(75,remaining_ms(deadline)));
        if(n==-2)continue;if(n<0){close_udp_socket(peer_socket);error="Tailscale discovery receive failed";return false;}if(n<=0||n>static_cast<int>(buffer.size()))continue;
        std::string id,session_id,host_public_key,host_nonce,signature;std::uint16_t host_peer_port=0;
        if(!parse_offer(std::string_view(reinterpret_cast<const char*>(buffer.data()),static_cast<std::size_t>(n)),id,session_id,host_public_key,host_nonce,host_peer_port,signature))continue;
        if(id!=connection_id)continue;if(connection_id_from_public_key(host_public_key)!=connection_id){rejection_error="Tailscale host identity mismatch";continue;}
        const auto transcript=offer_transcript(id,session_id,client_public_key,client_nonce,host_public_key,host_nonce,host_peer_port);if(!verify_hex(host_public_key,transcript,signature)){rejection_error="Tailscale offer signature invalid";continue;}
        UdpCandidate host;if(!candidate_from_udp(source,host)){rejection_error="Tailscale offer source invalid";continue;}host.port=host_peer_port;
        result.socket=peer_socket;peer_socket={};result.host=host;result.connection_id=id;result.session_id=session_id;result.host_public_key=host_public_key;result.client_nonce=client_nonce;result.host_nonce=host_nonce;return true;
    }
    close_udp_socket(peer_socket);error=rejection_error.empty()?"host not found on Tailscale":rejection_error;return false;
}

}
