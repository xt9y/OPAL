#include <opal/rendezvous_client.hpp>
#include <opal/crypto.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace opal { namespace {
using Clock=std::chrono::steady_clock;

bool same_source(const sockaddr_storage&a,const sockaddr_storage&b){
    if(a.ss_family!=b.ss_family)return false;
    if(a.ss_family==AF_INET6){const auto*x=reinterpret_cast<const sockaddr_in6*>(&a),*y=reinterpret_cast<const sockaddr_in6*>(&b);return x->sin6_port==y->sin6_port&&std::memcmp(&x->sin6_addr,&y->sin6_addr,sizeof(in6_addr))==0;}
    if(a.ss_family==AF_INET){const auto*x=reinterpret_cast<const sockaddr_in*>(&a),*y=reinterpret_cast<const sockaddr_in*>(&b);return x->sin_port==y->sin_port&&x->sin_addr.s_addr==y->sin_addr.s_addr;}
    return false;
}

int remaining_ms(Clock::time_point deadline){const auto now=Clock::now();if(now>=deadline)return 0;return std::max(1,static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count()));}
}

struct RendezvousClient::Impl {
    RendezvousConfig config;UdpSocket socket;sockaddr_storage server{};socklen_t server_len=0;
    bool send(const RendezvousMessage&m){const auto text=serialize_rendezvous_message(m);if(text.empty()||socket.fd<0)return false;return send_datagram(socket.fd,server,server_len,std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));}
    bool receive(RendezvousMessage&m,int timeout_ms,std::string&error){
        const auto deadline=Clock::now()+std::chrono::milliseconds(std::max(1,timeout_ms));std::array<std::uint8_t,kRendezvousMaxMessageBytes+1>buffer{};
        while(remaining_ms(deadline)>0){sockaddr_storage source{};socklen_t source_len=sizeof(source);const int n=recv_datagram(socket.fd,buffer,source,source_len,remaining_ms(deadline));if(n==-2)continue;if(n<0){error="rendezvous receive failed";return false;}if(!same_source(source,server))continue;if(n<=0||n>static_cast<int>(kRendezvousMaxMessageBytes))continue;const std::string_view wire(reinterpret_cast<const char*>(buffer.data()),static_cast<std::size_t>(n));if(!parse_rendezvous_message(wire,m))continue;if(m.type==RendezvousType::Error){error=m.error_code;return false;}return true;}
        error="rendezvous timeout";return false;
    }
};

RendezvousConfig default_rendezvous_config(){
    RendezvousConfig c;if(const char*h=std::getenv("OPAL_RENDEZVOUS_HOST");h&&*h)c.host=h;if(const char*p=std::getenv("OPAL_RENDEZVOUS_PORT");p&&*p)try{const int value=std::stoi(p);if(value>0&&value<=65535)c.port=static_cast<std::uint16_t>(value);}catch(...){ }return c;
}

RendezvousClient::RendezvousClient():impl_(std::make_unique<Impl>()){}
RendezvousClient::~RendezvousClient(){close();}

bool RendezvousClient::open(const RendezvousConfig&config){
    close();impl_=std::make_unique<Impl>();if(config.host.empty()||config.port==0)return false;impl_->config=config;impl_->socket=open_udp_socket();if(impl_->socket.fd<0)return false;if(!resolve_udp_endpoint(config.host,config.port,impl_->server,impl_->server_len)){close();return false;}return true;
}

bool RendezvousClient::register_host(const std::string&public_key,const std::filesystem::path&private_key,std::string&id,std::uint32_t&lease_seconds,std::string&error){
    id.clear();lease_seconds=0;error.clear();if(!valid()){error="rendezvous socket unavailable";return false;}id=rendezvous_id_from_public_key(public_key);if(id.empty()){error="invalid host identity";return false;}
    RendezvousMessage hello;hello.type=RendezvousType::LeaseHello;hello.id=id;hello.public_key=public_key;if(!impl_->send(hello)){error="rendezvous send failed";return false;}
    RendezvousMessage challenge;if(!impl_->receive(challenge,impl_->config.timeout_ms,error)||challenge.type!=RendezvousType::LeaseChallenge||challenge.id!=id){if(error.empty())error="rendezvous challenge protocol error";return false;}
    const auto signature=sign_hex(private_key,rendezvous_lease_transcript(id,public_key,challenge.nonce));if(signature.empty()){error="host identity signing failed";return false;}
    RendezvousMessage proof;proof.type=RendezvousType::LeaseProof;proof.id=id;proof.public_key=public_key;proof.nonce=challenge.nonce;proof.signature=signature;if(!impl_->send(proof)){error="rendezvous send failed";return false;}
    RendezvousMessage ok;if(!impl_->receive(ok,impl_->config.timeout_ms,error)||ok.type!=RendezvousType::LeaseOk||ok.id!=id){if(error.empty())error="rendezvous lease protocol error";return false;}lease_seconds=ok.ttl_seconds;return true;
}

bool RendezvousClient::wait_offer(RendezvousMessage&offer,int timeout_ms,std::string&error){
    error.clear();if(!valid()){error="rendezvous socket unavailable";return false;}RendezvousMessage message;if(!impl_->receive(message,timeout_ms,error))return false;if(message.type!=RendezvousType::Offer){error="unexpected rendezvous message";return false;}offer=std::move(message);return true;
}

bool RendezvousClient::accept_offer(const RendezvousMessage&offer,const std::string&host_public_key,const std::filesystem::path&host_private_key,RendezvousIntroduction&intro,std::string&error){
    intro={};error.clear();if(!valid()||offer.type!=RendezvousType::Offer){error="invalid rendezvous offer";return false;}const auto id=rendezvous_id_from_public_key(host_public_key);if(id.empty()||offer.id!=id){error="rendezvous offer identity mismatch";return false;}
    const auto host_nonce=random_hex(16);const auto signature=sign_hex(host_private_key,rendezvous_accept_transcript(id,offer.session_id,offer.public_key,offer.nonce,host_nonce));if(signature.empty()){error="host identity signing failed";return false;}
    RendezvousMessage accept;accept.type=RendezvousType::Accept;accept.id=id;accept.session_id=offer.session_id;accept.public_key=host_public_key;accept.nonce=host_nonce;accept.signature=signature;if(!impl_->send(accept)){error="rendezvous accept send failed";return false;}
    intro.rendezvous_id=id;intro.session_id=offer.session_id;intro.peer_public_key=offer.public_key;intro.local_nonce=host_nonce;intro.peer_nonce=offer.nonce;intro.peer_observed={offer.host,offer.port};return true;
}

bool RendezvousClient::introduce(const std::string&id,const std::string&client_public_key,RendezvousIntroduction&intro,std::string&error){
    intro={};error.clear();if(!valid()){error="rendezvous socket unavailable";return false;}std::string checked;if(!parse_connection_code(format_connection_code(id),checked)||checked!=id){error="invalid OPAL connection id";return false;}const auto client_nonce=random_hex(16);RendezvousMessage request;request.type=RendezvousType::Introduce;request.id=id;request.public_key=client_public_key;request.nonce=client_nonce;if(!impl_->send(request)){error="rendezvous send failed";return false;}
    const auto deadline=Clock::now()+std::chrono::milliseconds(std::max(1,impl_->config.timeout_ms*2));while(remaining_ms(deadline)>0){RendezvousMessage ready;if(!impl_->receive(ready,remaining_ms(deadline),error))return false;if(ready.type!=RendezvousType::Ready||ready.id!=id)continue;intro.rendezvous_id=id;intro.session_id=ready.session_id;intro.peer_public_key=ready.public_key;intro.local_nonce=client_nonce;intro.peer_nonce=ready.nonce;intro.peer_observed={ready.host,ready.port};return true;}error="rendezvous introduction timeout";return false;
}

UdpSocket RendezvousClient::take_socket(){if(!impl_)return {};UdpSocket result=impl_->socket;impl_->socket={};return result;}
std::uint16_t RendezvousClient::local_port()const{return impl_?impl_->socket.local_port:0;}
bool RendezvousClient::valid()const{return impl_&&impl_->socket.fd>=0&&impl_->server_len>0;}
void RendezvousClient::close(){if(impl_)close_udp_socket(impl_->socket);}

}
