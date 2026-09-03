#include <opal/rendezvous_server.hpp>
#include <opal/crypto.hpp>

#include <map>
#include <utility>

namespace opal {
namespace {
constexpr std::uint64_t kChallengeTtlMs=5000;
constexpr std::uint64_t kLeaseTtlMs=45000;
constexpr std::uint64_t kIntroTtlMs=12000;
constexpr std::uint64_t kAcceptedIntroTtlMs=30000;
constexpr std::uint64_t kRelayTtlMs=30000;
constexpr std::size_t kMaxChallenges=4096;
constexpr std::size_t kMaxLeases=65536;
constexpr std::size_t kMaxIntros=65536;
constexpr std::size_t kMaxRelays=65536;
constexpr std::uint32_t kRateLimitPer10s=120;

std::string endpoint_key(const RendezvousEndpoint&e){return e.host+"#"+std::to_string(e.port);}
RendezvousOutbound error_to(const RendezvousEndpoint&e,const char*code){RendezvousMessage m;m.type=RendezvousType::Error;m.error_code=code;return{e,std::move(m)};}
}

struct RendezvousServerState::Impl {
    struct Challenge {std::string public_key,nonce;RendezvousEndpoint endpoint;std::uint64_t expires=0;};
    struct Lease {std::string public_key;RendezvousEndpoint endpoint;std::uint64_t expires=0;};
    struct Intro {std::string id,session_id,client_public_key,client_nonce,host_public_key,host_nonce;RendezvousEndpoint client,host;std::uint64_t expires=0;bool accepted=false;};
    struct Relay {std::string allocation_id,session_id,client_public_key,host_public_key;RendezvousEndpoint client,host;std::uint64_t expires=0;bool client_ready=false,host_ready=false;};
    struct Rate {std::uint64_t start=0,last=0;std::uint32_t count=0;};
    RendezvousEndpoint relay_endpoint;
    std::map<std::string,Challenge> challenges;
    std::map<std::string,Lease> leases;
    std::map<std::string,Intro> intros;
    std::map<std::string,Relay> relays;
    std::map<std::string,std::string> relay_by_session;
    std::map<std::string,Rate> rates;

    explicit Impl(RendezvousEndpoint relay):relay_endpoint(std::move(relay)){}

    void cleanup(std::uint64_t now){
        for(auto it=challenges.begin();it!=challenges.end();)if(it->second.expires<=now)it=challenges.erase(it);else ++it;
        for(auto it=leases.begin();it!=leases.end();)if(it->second.expires<=now)it=leases.erase(it);else ++it;
        for(auto it=intros.begin();it!=intros.end();)if(it->second.expires<=now)it=intros.erase(it);else ++it;
        for(auto it=relays.begin();it!=relays.end();){if(it->second.expires<=now){relay_by_session.erase(it->second.session_id);it=relays.erase(it);}else ++it;}
        for(auto it=rates.begin();it!=rates.end();)if(now>it->second.last+60000)it=rates.erase(it);else ++it;
    }

    bool allow(const RendezvousEndpoint&e,std::uint64_t now){auto &r=rates[endpoint_key(e)];if(r.start==0||now>=r.start+10000){r.start=now;r.count=0;}r.last=now;if(r.count>=kRateLimitPer10s)return false;++r.count;return true;}
};

RendezvousServerState::RendezvousServerState():impl_(std::make_unique<Impl>(RendezvousEndpoint{})){}
RendezvousServerState::RendezvousServerState(RendezvousEndpoint relay_endpoint):impl_(std::make_unique<Impl>(std::move(relay_endpoint))){}
RendezvousServerState::~RendezvousServerState()=default;

void RendezvousServerState::cleanup(std::uint64_t now_ms){if(impl_)impl_->cleanup(now_ms);}
std::size_t RendezvousServerState::active_leases(std::uint64_t now_ms){if(!impl_)return 0;impl_->cleanup(now_ms);return impl_->leases.size();}
std::size_t RendezvousServerState::pending_introductions(std::uint64_t now_ms){if(!impl_)return 0;impl_->cleanup(now_ms);return impl_->intros.size();}

bool RendezvousServerState::relay_target(std::string_view allocation_id,RelayRole role,const RendezvousEndpoint&source,std::uint64_t now,RendezvousEndpoint&target){
    target={};if(!impl_||allocation_id.empty()||source.host.empty()||source.port==0)return false;impl_->cleanup(now);auto it=impl_->relays.find(std::string(allocation_id));if(it==impl_->relays.end())return false;const auto&r=it->second;if(!r.client_ready||!r.host_ready)return false;if(role==RelayRole::Client&&source==r.client){target=r.host;return true;}if(role==RelayRole::Host&&source==r.host){target=r.client;return true;}return false;
}

std::vector<RendezvousOutbound> RendezvousServerState::process(const RendezvousMessage&m,const RendezvousEndpoint&source,std::uint64_t now){
    std::vector<RendezvousOutbound> out;if(!impl_||source.host.empty()||source.port==0)return out;impl_->cleanup(now);
    if(!impl_->allow(source,now)){out.push_back(error_to(source,"RATE_LIMIT"));return out;}
    try{
        switch(m.type){
        case RendezvousType::LeaseHello:{
            if(impl_->challenges.size()>=kMaxChallenges&&!impl_->challenges.count(m.id)){out.push_back(error_to(source,"BUSY"));break;}
            const auto nonce=random_hex(16);impl_->challenges[m.id]={m.public_key,nonce,source,now+kChallengeTtlMs};RendezvousMessage reply;reply.type=RendezvousType::LeaseChallenge;reply.id=m.id;reply.nonce=nonce;out.push_back({source,std::move(reply)});break;
        }
        case RendezvousType::LeaseProof:{
            auto it=impl_->challenges.find(m.id);if(it==impl_->challenges.end()){out.push_back(error_to(source,"CHALLENGE_EXPIRED"));break;}const auto challenge=it->second;const bool endpoint_ok=challenge.endpoint==source,fields_ok=challenge.public_key==m.public_key&&challenge.nonce==m.nonce;const bool signature_ok=endpoint_ok&&fields_ok&&verify_hex(m.public_key,rendezvous_lease_transcript(m.id,m.public_key,m.nonce),m.signature);impl_->challenges.erase(it);if(!signature_ok){out.push_back(error_to(source,"AUTH_FAILED"));break;}auto existing=impl_->leases.find(m.id);if(existing!=impl_->leases.end()&&existing->second.public_key!=m.public_key){out.push_back(error_to(source,"ID_COLLISION"));break;}if(impl_->leases.size()>=kMaxLeases&&existing==impl_->leases.end()){out.push_back(error_to(source,"BUSY"));break;}impl_->leases[m.id]={m.public_key,source,now+kLeaseTtlMs};RendezvousMessage reply;reply.type=RendezvousType::LeaseOk;reply.id=m.id;reply.ttl_seconds=static_cast<std::uint32_t>(kLeaseTtlMs/1000);out.push_back({source,std::move(reply)});break;
        }
        case RendezvousType::Introduce:{
            auto lease=impl_->leases.find(m.id);if(lease==impl_->leases.end()){out.push_back(error_to(source,"HOST_OFFLINE"));break;}if(impl_->intros.size()>=kMaxIntros){out.push_back(error_to(source,"BUSY"));break;}const auto session=random_hex(16);Impl::Intro intro{m.id,session,m.public_key,m.nonce,lease->second.public_key,{},source,lease->second.endpoint,now+kIntroTtlMs,false};impl_->intros[session]=intro;RendezvousMessage offer;offer.type=RendezvousType::Offer;offer.id=m.id;offer.session_id=session;offer.public_key=m.public_key;offer.host=source.host;offer.port=source.port;offer.nonce=m.nonce;out.push_back({lease->second.endpoint,std::move(offer)});break;
        }
        case RendezvousType::Accept:{
            auto it=impl_->intros.find(m.session_id);if(it==impl_->intros.end()){out.push_back(error_to(source,"SESSION_EXPIRED"));break;}auto &intro=it->second;const bool fields_ok=intro.id==m.id&&intro.host_public_key==m.public_key&&intro.host==source;const bool signature_ok=fields_ok&&verify_hex(m.public_key,rendezvous_accept_transcript(m.id,m.session_id,intro.client_public_key,intro.client_nonce,m.nonce),m.signature);if(!signature_ok){out.push_back(error_to(source,"AUTH_FAILED"));break;}intro.host_nonce=m.nonce;intro.accepted=true;intro.expires=now+kAcceptedIntroTtlMs;RendezvousMessage ready;ready.type=RendezvousType::Ready;ready.id=intro.id;ready.session_id=intro.session_id;ready.public_key=intro.host_public_key;ready.host=source.host;ready.port=source.port;ready.nonce=intro.host_nonce;out.push_back({intro.client,std::move(ready)});break;
        }
        case RendezvousType::RelayRequest:{
            if(impl_->relay_endpoint.host.empty()||impl_->relay_endpoint.port==0){out.push_back(error_to(source,"RELAY_UNAVAILABLE"));break;}auto intro_it=impl_->intros.find(m.session_id);if(intro_it==impl_->intros.end()||!intro_it->second.accepted){out.push_back(error_to(source,"SESSION_EXPIRED"));break;}auto &intro=intro_it->second;const bool is_client=m.public_key==intro.client_public_key&&source==intro.client;const bool is_host=m.public_key==intro.host_public_key&&source==intro.host;const auto transcript=relay_request_transcript(m.session_id,m.public_key,m.nonce);if((!is_client&&!is_host)||transcript.empty()||!verify_hex(m.public_key,transcript,m.signature)){out.push_back(error_to(source,"AUTH_FAILED"));break;}std::string allocation;auto by_session=impl_->relay_by_session.find(m.session_id);if(by_session!=impl_->relay_by_session.end())allocation=by_session->second;if(allocation.empty()){if(impl_->relays.size()>=kMaxRelays){out.push_back(error_to(source,"BUSY"));break;}allocation=random_hex(16);impl_->relays[allocation]={allocation,m.session_id,intro.client_public_key,intro.host_public_key,intro.client,intro.host,now+kRelayTtlMs,false,false};impl_->relay_by_session[m.session_id]=allocation;}auto relay_it=impl_->relays.find(allocation);if(relay_it==impl_->relays.end()){out.push_back(error_to(source,"INTERNAL"));break;}auto &relay=relay_it->second;relay.expires=now+kRelayTtlMs;if(is_client)relay.client_ready=true;if(is_host)relay.host_ready=true;RendezvousMessage reply;reply.type=RendezvousType::RelayReady;reply.session_id=m.session_id;reply.host=impl_->relay_endpoint.host;reply.port=impl_->relay_endpoint.port;reply.allocation_id=relay.allocation_id;reply.ttl_seconds=static_cast<std::uint32_t>(kRelayTtlMs/1000);out.push_back({source,std::move(reply)});break;
        }
        default:out.push_back(error_to(source,"BAD_REQUEST"));break;
        }
    }catch(...){out.clear();out.push_back(error_to(source,"INTERNAL"));}
    return out;
}

}
