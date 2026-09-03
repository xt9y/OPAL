#include <opal/direct_video_session.hpp>
#include <opal/video_packet.hpp>
#include <algorithm>
#include <chrono>
#include <netdb.h>
#include <openssl/evp.h>
#include <sstream>
#include <utility>

namespace opal {
namespace {
using Clock=std::chrono::steady_clock;
constexpr char kUnavailable[]="Direct UDP video could not be established. This network/NAT does not permit OPAL's direct-only video path.";

int remaining_ms(Clock::time_point deadline){
    const auto now=Clock::now();
    if(now>=deadline)return 0;
    return std::max(1,static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count()));
}

std::uint64_t make_session_id(const std::string &token,const std::string &pub,
                              const std::string &fingerprint,std::uint32_t generation){
    const std::string input=token+"\n"+pub+"\n"+fingerprint+"\n"+std::to_string(generation);
    unsigned char digest[32]{};unsigned int length=0;
    if(EVP_Digest(input.data(),input.size(),digest,&length,EVP_sha256(),nullptr)!=1||length<8)return 0;
    std::uint64_t value=0;for(int i=0;i<8;++i)value=(value<<8)|digest[i];return value?value:1;
}

bool parse_candidate_line(const std::string &line,std::uint32_t generation,
                          UdpCandidate &candidate,bool &done){
    done=false;std::istringstream stream(line);std::string word,type,host,extra;unsigned long long gen=0,port=0;
    if(!(stream>>word>>gen))return false;
    if(word=="UDP_CANDIDATES_DONE"){
        if(gen!=generation||stream>>extra)return false;
        done=true;return true;
    }
    if(word!="UDP_CANDIDATE"||gen!=generation||!(stream>>type>>host>>port)||stream>>extra)return false;
    if((type!="L"&&type!="S")||host.empty()||host.size()>255||port==0||port>65535)return false;
    candidate={host,static_cast<std::uint16_t>(port),type=="L"?CandidateType::Local:CandidateType::ServerReflexive};
    return true;
}

bool parse_ready(const std::string &line,std::uint32_t generation){
    std::istringstream stream(line);std::string word,extra;unsigned long long gen=0;
    return (stream>>word>>gen)&&word=="UDP_PROBE_READY"&&gen==generation&&!(stream>>extra);
}

bool parse_selected(const std::string &line,std::uint32_t generation){
    std::istringstream stream(line);std::string word,host,extra;unsigned long long gen=0,port=0;
    return (stream>>word>>gen>>host>>port)&&word=="UDP_SELECTED"&&gen==generation&&
           !host.empty()&&host.size()<=255&&port>0&&port<=65535&&!(stream>>extra);
}

std::string numeric_address(const sockaddr_storage &address,socklen_t length,std::uint16_t &port){
    char host[NI_MAXHOST]{},service[NI_MAXSERV]{};
    if(getnameinfo(reinterpret_cast<const sockaddr*>(&address),length,host,sizeof(host),service,sizeof(service),
                   NI_NUMERICHOST|NI_NUMERICSERV)!=0)return {};
    try{const int parsed=std::stoi(service);if(parsed<1||parsed>65535)return {};port=static_cast<std::uint16_t>(parsed);}
    catch(...){return {};}
    return host;
}

std::vector<std::uint8_t> secure_probe(VideoMediaType type,const VideoKeys &keys,
                                       std::uint32_t generation,std::uint64_t session_id,
                                       std::uint64_t &sequence){
    VideoPacketHeader header;header.media_type=type;header.generation=generation;header.session_id=session_id;
    header.packet_sequence=sequence++;header.payload_length=1;
    const auto aad=serialize_video_header(header);const std::uint8_t value=type==VideoMediaType::Probe?'P':'A';
    std::vector<std::uint8_t> ciphertext;
    if(!seal_video_datagram(keys,header.packet_sequence,aad,std::span<const std::uint8_t>(&value,1),ciphertext))return {};
    std::vector<std::uint8_t> wire(aad.begin(),aad.end());wire.insert(wire.end(),ciphertext.begin(),ciphertext.end());return wire;
}

bool open_probe(std::span<const std::uint8_t> wire,const VideoKeys &keys,
                std::uint32_t generation,std::uint64_t session_id,
                VideoMediaType &type,ReplayWindow1024 &replay){
    if(wire.size()<kVideoHeaderBytes+kVideoAeadTagBytes)return false;
    VideoPacketHeader header;
    if(!parse_video_header(wire,header)||header.generation!=generation||header.session_id!=session_id||
       (header.media_type!=VideoMediaType::Probe&&header.media_type!=VideoMediaType::ProbeAck)||
       wire.size()!=kVideoHeaderBytes+header.payload_length+kVideoAeadTagBytes)return false;
    std::vector<std::uint8_t> plaintext;
    if(!open_video_datagram(keys,header.packet_sequence,wire.first(kVideoHeaderBytes),
                            wire.subspan(kVideoHeaderBytes),plaintext))return false;
    const std::uint8_t expected=header.media_type==VideoMediaType::Probe?'P':'A';
    if(plaintext.size()!=1||plaintext[0]!=expected)return false;
    if(!replay.accept(header.packet_sequence))return false;
    type=header.media_type;return true;
}

bool negotiate(bool client_side,SSL *ssl,const std::string &token,const std::string &pub,
               const std::string &fingerprint,std::uint32_t generation,
               const std::vector<StunEndpoint> &stun,ControlSend control_send,ControlRead control_read,
               DirectVideoPath &output,std::string &error,int deadline_ms){
    error.clear();DirectVideoPath path;
    const auto deadline=Clock::now()+std::chrono::milliseconds(std::max(1,deadline_ms));
    path.socket=open_udp_socket();if(path.socket.fd<0){error=kUnavailable;return false;}

    auto local=local_udp_candidates(path.socket);if(local.size()>15)local.resize(15);
    if(!stun.empty()&&local.size()<16){
        const int remaining=remaining_ms(deadline);if(!remaining){error=kUnavailable;return false;}
        if(auto reflexive=discover_server_reflexive_candidate(path.socket,stun,std::min(750,remaining)))local.push_back(*reflexive);
    }
    for(const auto &candidate:local){
        const int remaining=remaining_ms(deadline);if(!remaining){error=kUnavailable;return false;}
        const std::string line="UDP_CANDIDATE "+std::to_string(generation)+" "+
            (candidate.type==CandidateType::Local?"L ":"S ")+candidate.host+" "+std::to_string(candidate.port);
        if(!control_send(line,std::min(500,remaining))){error=kUnavailable;return false;}
    }
    {const int remaining=remaining_ms(deadline);if(!remaining||!control_send("UDP_CANDIDATES_DONE "+std::to_string(generation),std::min(500,remaining))){error=kUnavailable;return false;}}

    std::vector<UdpCandidate> remote;bool done=false;
    while(!done){
        const int remaining=remaining_ms(deadline);if(!remaining){error=kUnavailable;return false;}
        std::string line;if(!control_read(line,remaining)){error=kUnavailable;return false;}
        UdpCandidate candidate;
        if(!parse_candidate_line(line,generation,candidate,done)){error="direct UDP negotiation protocol error";return false;}
        if(!done){if(remote.size()>=16){error="direct UDP negotiation protocol error";return false;}remote.push_back(std::move(candidate));}
    }
    if(remote.empty()){error=kUnavailable;return false;}
    if(!derive_video_keys(ssl,token,pub,fingerprint,client_side,path.keys)){error="direct UDP key derivation failed";return false;}
    path.session_id=make_session_id(token,pub,fingerprint,generation);if(!path.session_id){error="direct UDP key derivation failed";return false;}
    path.generation=generation;
    {const int remaining=remaining_ms(deadline);if(!remaining||!control_send("UDP_PROBE_READY "+std::to_string(generation),std::min(500,remaining))){error=kUnavailable;return false;}}
    for(;;){
        const int remaining=remaining_ms(deadline);if(!remaining){error=kUnavailable;return false;}
        std::string line;if(!control_read(line,remaining)){error=kUnavailable;return false;}
        if(!parse_ready(line,generation)){error="direct UDP negotiation protocol error";return false;}break;
    }

    std::vector<std::pair<sockaddr_storage,socklen_t>> targets;
    for(const auto &candidate:remote){sockaddr_storage address{};socklen_t length=0;if(resolve_udp_endpoint(candidate.host,candidate.port,address,length))targets.push_back({address,length});}
    if(targets.empty()){error=kUnavailable;return false;}

    ReplayWindow1024 replay;std::uint64_t sequence=1;auto next_probe=Clock::now();
    bool local_selected=false,peer_selected=false,selected_sent=false;sockaddr_storage selected{};socklen_t selected_length=0;
    while(remaining_ms(deadline)>0){
        const auto now=Clock::now();
        if(now>=next_probe&&!local_selected){auto probe=secure_probe(VideoMediaType::Probe,path.keys,generation,path.session_id,sequence);for(const auto &target:targets)send_datagram(path.socket.fd,target.first,target.second,probe);next_probe=now+std::chrono::milliseconds(50);}
        std::uint8_t wire[kVideoMaxDatagramBytes+1]{};sockaddr_storage source{};socklen_t source_length=sizeof(source);
        const int received=recv_datagram(path.socket.fd,wire,source,source_length,std::min(5,remaining_ms(deadline)));
        if(received>0&&received<=static_cast<int>(kVideoMaxDatagramBytes)){
            VideoMediaType type;
            if(open_probe(std::span<const std::uint8_t>(wire,static_cast<std::size_t>(received)),path.keys,generation,path.session_id,type,replay)){
                if(type==VideoMediaType::Probe){auto ack=secure_probe(VideoMediaType::ProbeAck,path.keys,generation,path.session_id,sequence);send_datagram(path.socket.fd,source,source_length,ack);}
                else if(!local_selected){selected=source;selected_length=source_length;local_selected=true;}
            }
        }
        if(local_selected&&!selected_sent){
            std::uint16_t port=0;const auto host=numeric_address(selected,selected_length,port);const int remaining=remaining_ms(deadline);
            if(host.empty()||!remaining||!control_send("UDP_SELECTED "+std::to_string(generation)+" "+host+" "+std::to_string(port),std::min(200,remaining))){error=kUnavailable;return false;}
            selected_sent=true;
        }
        std::string control_line;
        if(control_read(control_line,1)){
            if(!parse_selected(control_line,generation)){error="direct UDP negotiation protocol error";return false;}
            peer_selected=true;
        }
        if(local_selected&&peer_selected){path.peer=selected;path.peer_len=selected_length;output=std::move(path);return true;}
    }
    error=kUnavailable;return false;
}
}

DirectVideoPath::~DirectVideoPath(){close_udp_socket(socket);}
DirectVideoPath::DirectVideoPath(DirectVideoPath &&other) noexcept{*this=std::move(other);}
DirectVideoPath& DirectVideoPath::operator=(DirectVideoPath &&other) noexcept{
    if(this!=&other){
        close_udp_socket(socket);socket=other.socket;other.socket={};peer=other.peer;peer_len=other.peer_len;other.peer_len=0;
        keys=other.keys;session_id=other.session_id;other.session_id=0;generation=other.generation;other.generation=0;
    }
    return *this;
}
const char* direct_video_unavailable_error(){return kUnavailable;}

bool negotiate_client_direct_video(SSL *ssl,const std::string &token,const std::string &pub,
    const std::string &fingerprint,std::uint32_t generation,const std::vector<StunEndpoint> &stun,
    ControlSend send,ControlRead read,DirectVideoPath &path,std::string &error,int deadline_ms){
    return negotiate(true,ssl,token,pub,fingerprint,generation,stun,std::move(send),std::move(read),path,error,deadline_ms);
}
bool negotiate_host_direct_video(SSL *ssl,const std::string &token,const std::string &pub,
    const std::string &fingerprint,std::uint32_t generation,const std::vector<StunEndpoint> &stun,
    ControlSend send,ControlRead read,DirectVideoPath &path,std::string &error,int deadline_ms){
    return negotiate(false,ssl,token,pub,fingerprint,generation,stun,std::move(send),std::move(read),path,error,deadline_ms);
}
}
