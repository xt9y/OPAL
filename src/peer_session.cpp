#include <opal/peer_session.hpp>
#include <opal/crypto.hpp>
#include <opal/reliable_control.hpp>
#include <opal/session_packet.hpp>
#include <opal/video_packet.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <netinet/in.h>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace opal { namespace {
using Clock=std::chrono::steady_clock;
std::uint64_t monotonic_ms(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());}
int remaining_ms(Clock::time_point deadline){const auto now=Clock::now();if(now>=deadline)return 0;return std::max(1,static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count()));}
std::uint32_t read_magic(std::span<const std::uint8_t>b){if(b.size()<4)return 0;return(static_cast<std::uint32_t>(b[0])<<24)|(static_cast<std::uint32_t>(b[1])<<16)|(static_cast<std::uint32_t>(b[2])<<8)|b[3];}
std::uint64_t numeric_session_id(const std::string&hex_id){const auto bytes=unhex(hex_id);if(bytes.size()!=16)return 0;std::uint64_t value=0;for(int i=0;i<8;++i)value=(value<<8)|bytes[static_cast<std::size_t>(i)];return value?value:1;}
std::vector<std::string> fields(std::string_view text){std::istringstream in{std::string(text)};std::vector<std::string>out;std::string v;while(in>>v)out.push_back(std::move(v));return out;}
VideoKeys channel_video_keys(const PeerChannelKeys&keys){VideoKeys result;result.send_key=keys.send_key;result.recv_key=keys.recv_key;result.send_nonce_base=keys.send_nonce_base;result.recv_nonce_base=keys.recv_nonce_base;return result;}
bool same_source(const sockaddr_storage&a,const sockaddr_storage&b){if(a.ss_family!=b.ss_family)return false;if(a.ss_family==AF_INET6){const auto*x=reinterpret_cast<const sockaddr_in6*>(&a),*y=reinterpret_cast<const sockaddr_in6*>(&b);return x->sin6_port==y->sin6_port&&std::memcmp(&x->sin6_addr,&y->sin6_addr,sizeof(in6_addr))==0;}if(a.ss_family==AF_INET){const auto*x=reinterpret_cast<const sockaddr_in*>(&a),*y=reinterpret_cast<const sockaddr_in*>(&b);return x->sin_port==y->sin_port&&x->sin_addr.s_addr==y->sin_addr.s_addr;}return false;}
bool retryable_path_error(const std::string&e){return e.find("timed out")!=std::string::npos||e.find("receive")!=std::string::npos||e.find("send failed")!=std::string::npos||e.find("confirmation")!=std::string::npos;}
bool usable_endpoint(const RendezvousEndpoint&e){return !e.host.empty()&&e.port>0;}
bool same_endpoint(const RendezvousEndpoint&a,const RendezvousEndpoint&b){return a.host==b.host&&a.port==b.port;}
constexpr std::size_t kPeerMediaQueueCapacity=32;
constexpr std::size_t kPeerControlQueueCapacity=64;
}

struct PeerSession::Impl {
    struct MediaSlot{std::array<std::uint8_t,kVideoMaxDatagramBytes+1>bytes{};std::size_t size=0;};
    struct ControlSlot{SessionPacketType type=SessionPacketType::ControlAck;std::string payload;};

    PeerSessionOptions options;RendezvousEndpoint active_endpoint;sockaddr_storage peer{};socklen_t peer_len=0;std::uint64_t session_numeric=0;bool relay_mode=false;
    PeerEphemeralKey ephemeral;PeerSessionKeys keys;std::unique_ptr<VideoCipher>cipher;ReplayWindow1024 replay;
    ReliableControlSender reliable_sender;ReliableControlReceiver reliable_receiver;LatestPointerReceiver pointer_receiver;
    std::thread thread,media_thread,control_thread;mutable std::mutex send_mu,state_mu,reliable_mu,media_mu,control_mu;std::condition_variable media_cv,control_cv;
    std::array<MediaSlot,kPeerMediaQueueCapacity>media_queue{};std::size_t media_head=0,media_count=0;
    std::array<ControlSlot,kPeerControlQueueCapacity>control_queue{};std::size_t control_head=0,control_count=0;
    std::atomic<bool>run{false},established{false};std::atomic<std::uint64_t>pointer_send_sequence{0},reliable_pending_count{0},media_drop_count{0},control_drop_count{0},pointer_overwrite_count{0};std::uint64_t packet_send_sequence=1;std::string error,path="none";bool dropped_test_reliable=false;

    void wake_workers(){media_cv.notify_all();control_cv.notify_all();}
    void set_error(std::string text){std::lock_guard<std::mutex>lock(state_mu);error=std::move(text);}
    void clear_media_queue(){std::lock_guard<std::mutex>lock(media_mu);for(auto&slot:media_queue)slot.size=0;media_head=0;media_count=0;}
    void clear_control_queue(){std::lock_guard<std::mutex>lock(control_mu);for(auto&slot:control_queue)slot.payload.clear();control_head=0;control_count=0;}
    void reset_attempt(){clear_peer_ephemeral(ephemeral);clear_peer_session_keys(keys);cipher.reset();replay.reset();reliable_sender.reset();reliable_receiver.reset();pointer_receiver.reset();clear_media_queue();clear_control_queue();packet_send_sequence=1;pointer_send_sequence.store(0);reliable_pending_count.store(0);media_drop_count.store(0);control_drop_count.store(0);pointer_overwrite_count.store(0);established.store(false);dropped_test_reliable=false;}
    bool activate(const RendezvousEndpoint&endpoint,bool relay){active_endpoint=endpoint;relay_mode=relay;peer={};peer_len=0;return !endpoint.host.empty()&&endpoint.port>0&&resolve_udp_endpoint(endpoint.host,endpoint.port,peer,peer_len);}
    bool send_wire(std::span<const std::uint8_t>wire){if(options.socket.fd<0||wire.empty())return false;if(relay_mode){if(!options.relay)return false;const auto wrapped=wrap_relay_datagram(options.relay->allocation_id,options.relay->role,wire);return !wrapped.empty()&&send_datagram(options.socket.fd,peer,peer_len,wrapped);}return send_datagram(options.socket.fd,peer,peer_len,wire);}
    bool receive_wire(std::span<std::uint8_t>buffer,std::span<const std::uint8_t>&wire,int timeout_ms){sockaddr_storage source{};socklen_t source_len=sizeof(source);const int n=recv_datagram(options.socket.fd,buffer,source,source_len,timeout_ms);if(n<=0||!same_source(source,peer))return false;wire=std::span<const std::uint8_t>(buffer.data(),static_cast<std::size_t>(n));return true;}
    bool send_plain(SessionPacketType type,std::string_view payload,std::uint64_t packet_sequence){SessionPacketHeader h;h.type=type;h.generation=options.handshake.generation;h.session_id=session_numeric;h.packet_sequence=packet_sequence;h.payload_length=static_cast<std::uint16_t>(payload.size());const auto header=serialize_session_header(h);if(header.empty()||payload.size()>kSessionPacketMaxPayload)return false;std::vector<std::uint8_t>wire=header;if(!payload.empty())wire.insert(wire.end(),reinterpret_cast<const std::uint8_t*>(payload.data()),reinterpret_cast<const std::uint8_t*>(payload.data()+payload.size()));return send_wire(wire);}
    bool install_keys(const std::string&client_ephemeral,const std::string&host_ephemeral){if(!derive_peer_session_keys(options.handshake,ephemeral,client_ephemeral,host_ephemeral,options.client_side,keys))return false;cipher=std::make_unique<VideoCipher>(channel_video_keys(keys.control));replay.reset();return cipher&&cipher->valid();}
    bool send_encrypted(SessionPacketType type,std::uint64_t reliable_sequence,std::string_view payload){if(!cipher||payload.size()>kSessionPacketMaxPayload)return false;std::lock_guard<std::mutex>send_lock(send_mu);ReliableAckState ack;{std::lock_guard<std::mutex>lock(reliable_mu);ack=reliable_receiver.ack_state();}SessionPacketHeader h;h.type=type;h.generation=options.handshake.generation;h.session_id=session_numeric;h.packet_sequence=packet_send_sequence++;h.reliable_sequence=reliable_sequence;h.ack_sequence=ack.sequence;h.ack_bits=ack.bits;h.payload_length=static_cast<std::uint16_t>(payload.size());const auto aad=serialize_session_header(h);if(aad.empty())return false;std::vector<std::uint8_t>sealed(payload.size()+16);std::size_t sealed_size=0;if(!cipher->seal(h.packet_sequence,aad,std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(payload.data()),payload.size()),sealed,sealed_size))return false;sealed.resize(sealed_size);std::vector<std::uint8_t>wire=aad;wire.insert(wire.end(),sealed.begin(),sealed.end());if(type==SessionPacketType::ReliableControl){const char*drop=std::getenv("OPAL_TEST_DROP_FIRST_RELIABLE");if(drop&&*drop&&std::string(drop)!="0"&&!dropped_test_reliable){dropped_test_reliable=true;return true;}}return send_wire(wire);}
    bool open_encrypted(std::span<const std::uint8_t>wire,SessionPacketHeader*out_header,std::string*out_payload,SessionPacketType require=static_cast<SessionPacketType>(0)){if(!cipher||wire.size()<kSessionPacketHeaderBytes+16)return false;SessionPacketHeader h;if(!parse_session_header(wire,h)||h.generation!=options.handshake.generation||h.session_id!=session_numeric||wire.size()!=kSessionPacketHeaderBytes+h.payload_length+16)return false;if(static_cast<std::uint8_t>(require)!=0&&h.type!=require)return false;std::vector<std::uint8_t>plain(h.payload_length);std::size_t plain_size=0;if(!cipher->open(h.packet_sequence,wire.first(kSessionPacketHeaderBytes),wire.subspan(kSessionPacketHeaderBytes),plain,plain_size)||plain_size!=h.payload_length||!replay.accept(h.packet_sequence))return false;if(out_header)*out_header=h;if(out_payload)out_payload->assign(reinterpret_cast<const char*>(plain.data()),plain.size());return true;}

    bool client_handshake(int timeout_ms,std::string&err){
        if(!generate_peer_ephemeral(ephemeral)){err="ephemeral key generation failed";return false;}const auto client_ephemeral=peer_ephemeral_public_hex(ephemeral);const auto transcript=peer_client_hello_transcript(options.handshake,client_ephemeral);const auto signature=sign_hex(options.identity_private_key,transcript);if(signature.empty()){err="client identity signing failed";return false;}const std::string proof=options.handshake.auth_binding=="pairing"?peer_pairing_proof(options.pairing_password,options.handshake,client_ephemeral):"-";if(options.handshake.auth_binding=="pairing"&&proof.empty()){err="pairing proof unavailable";return false;}const std::string hello="HELLO "+options.handshake.client_identity+" "+client_ephemeral+" "+signature+" "+proof;const auto deadline=Clock::now()+std::chrono::milliseconds(std::max(1,timeout_ms));std::uint64_t sequence=1;std::array<std::uint8_t,kVideoMaxDatagramBytes+1>buffer{};auto next_send=Clock::time_point{};
        while(remaining_ms(deadline)>0){const auto now=Clock::now();if(next_send.time_since_epoch().count()==0||now>=next_send){if(!send_plain(SessionPacketType::HandshakeClient,hello,sequence++)){err="client hello send failed";return false;}next_send=now+std::chrono::milliseconds(100);}std::span<const std::uint8_t>wire;if(!receive_wire(buffer,wire,std::min(50,remaining_ms(deadline))))continue;SessionPacketHeader h;if(!parse_session_header(wire,h)||h.type!=SessionPacketType::HandshakeHost||h.generation!=options.handshake.generation||h.session_id!=session_numeric||wire.size()!=kSessionPacketHeaderBytes+h.payload_length)continue;const std::string payload(reinterpret_cast<const char*>(wire.data()+kSessionPacketHeaderBytes),h.payload_length);const auto f=fields(payload);if(f.size()!=5||f[0]!="WELCOME"||f[1]!=options.handshake.host_identity){err="host identity mismatch";return false;}const auto welcome=peer_host_welcome_transcript(options.handshake,client_ephemeral,f[2]);if(welcome.empty()||!verify_hex(options.handshake.host_identity,welcome,f[3])){err="host handshake signature invalid";return false;}if(!install_keys(client_ephemeral,f[2])){err="peer key derivation failed";return false;}if(peer_confirmation_mac(keys,welcome)!=f[4]){err="host key confirmation failed";return false;}const auto finish=peer_confirmation_mac(keys,"CLIENT\n"+welcome);for(int attempt=0;attempt<8&&remaining_ms(deadline)>0;++attempt){if(!send_plain(SessionPacketType::HandshakeFinish,"FINISH "+finish,sequence++)){err="client finish send failed";return false;}const auto wait_until=Clock::now()+std::chrono::milliseconds(150);while(Clock::now()<wait_until&&remaining_ms(deadline)>0){std::span<const std::uint8_t>reply;if(!receive_wire(buffer,reply,std::min(50,remaining_ms(deadline))))continue;if(open_encrypted(reply,nullptr,nullptr,SessionPacketType::Keepalive)){established.store(true);return true;}}}err="host finish confirmation timed out";return false;}err="peer handshake timed out";return false;
    }

    bool host_handshake(int timeout_ms,std::string&err){
        if(!generate_peer_ephemeral(ephemeral)){err="ephemeral key generation failed";return false;}const auto host_ephemeral=peer_ephemeral_public_hex(ephemeral);const auto deadline=Clock::now()+std::chrono::milliseconds(std::max(1,timeout_ms));std::array<std::uint8_t,kVideoMaxDatagramBytes+1>buffer{};std::string client_ephemeral,welcome,welcome_payload;std::uint64_t sequence=1;auto next_probe=Clock::time_point{};
        while(remaining_ms(deadline)>0){const auto now=Clock::now();if(next_probe.time_since_epoch().count()==0||now>=next_probe){(void)send_plain(SessionPacketType::PathProbe,"P",sequence++);next_probe=now+std::chrono::milliseconds(100);}std::span<const std::uint8_t>wire;if(!receive_wire(buffer,wire,std::min(100,remaining_ms(deadline))))continue;SessionPacketHeader h;if(!parse_session_header(wire,h)||h.generation!=options.handshake.generation||h.session_id!=session_numeric||wire.size()!=kSessionPacketHeaderBytes+h.payload_length)continue;const std::string payload(reinterpret_cast<const char*>(wire.data()+kSessionPacketHeaderBytes),h.payload_length);if(h.type==SessionPacketType::HandshakeClient){const auto f=fields(payload);if(f.size()!=5||f[0]!="HELLO"||f[1]!=options.handshake.client_identity)continue;client_ephemeral=f[2];const auto hello=peer_client_hello_transcript(options.handshake,client_ephemeral);if(hello.empty()||!verify_hex(options.handshake.client_identity,hello,f[3]))continue;if(options.handshake.auth_binding=="pairing"){const auto expected=peer_pairing_proof(options.pairing_password,options.handshake,client_ephemeral);if(expected.empty()||f[4]!=expected)continue;}else if(f[4]!="-")continue;if(!install_keys(client_ephemeral,host_ephemeral)){err="peer key derivation failed";return false;}welcome=peer_host_welcome_transcript(options.handshake,client_ephemeral,host_ephemeral);const auto host_signature=sign_hex(options.identity_private_key,welcome);const auto confirmation=peer_confirmation_mac(keys,welcome);if(host_signature.empty()||confirmation.empty()){err="host handshake signing failed";return false;}welcome_payload="WELCOME "+options.handshake.host_identity+" "+host_ephemeral+" "+host_signature+" "+confirmation;if(!send_plain(SessionPacketType::HandshakeHost,welcome_payload,sequence++)){err="host welcome send failed";return false;}continue;}if(h.type==SessionPacketType::HandshakeFinish&&!welcome.empty()){const auto f=fields(payload);if(f.size()!=2||f[0]!="FINISH"||f[1]!=peer_confirmation_mac(keys,"CLIENT\n"+welcome))continue;established.store(true);if(!send_encrypted(SessionPacketType::Keepalive,0,"")){err="host confirmation send failed";return false;}return true;}}err="peer handshake timed out";return false;
    }

    bool attempt(const RendezvousEndpoint&endpoint,bool relay,int timeout_ms,std::string&err){reset_attempt();if(!activate(endpoint,relay)){err=relay?"relay endpoint resolution failed":"peer endpoint resolution failed";return false;}return options.client_side?client_handshake(timeout_ms,err):host_handshake(timeout_ms,err);}
    void apply_ack(const SessionPacketHeader&h){{std::lock_guard<std::mutex>lock(reliable_mu);reliable_sender.acknowledge({h.ack_sequence,h.ack_bits});reliable_pending_count.store(reliable_sender.pending());}}

    bool enqueue_control(SessionPacketType type,std::string payload){
        if(type!=SessionPacketType::ReliableControl&&type!=SessionPacketType::Pointer)return true;
        {
            std::lock_guard<std::mutex>lock(control_mu);
            if(type==SessionPacketType::Pointer){
                for(std::size_t i=0;i<control_count;++i){const auto index=(control_head+i)%control_queue.size();if(control_queue[index].type==SessionPacketType::Pointer){control_queue[index].payload=std::move(payload);pointer_overwrite_count.fetch_add(1);control_cv.notify_one();return true;}}
            }
            if(control_count>=control_queue.size()){control_drop_count.fetch_add(1);return false;}
            const auto tail=(control_head+control_count)%control_queue.size();control_queue[tail].type=type;control_queue[tail].payload=std::move(payload);++control_count;
        }
        control_cv.notify_one();return true;
    }

    void process_control(const SessionPacketHeader&h,std::string payload){
        apply_ack(h);
        if(h.type==SessionPacketType::ReliableControl){
            std::vector<std::string>delivered;{std::lock_guard<std::mutex>lock(reliable_mu);reliable_receiver.receive(h.reliable_sequence,std::move(payload),delivered);}
            for(auto&command:delivered){if(!enqueue_control(SessionPacketType::ReliableControl,std::move(command))){set_error("control dispatch queue overflow");run.store(false);wake_workers();return;}}
            (void)send_encrypted(SessionPacketType::ControlAck,0,"");
        }else if(h.type==SessionPacketType::Pointer){
            bool accepted=false;std::string latest;{std::lock_guard<std::mutex>lock(reliable_mu);accepted=pointer_receiver.accept(h.reliable_sequence,std::move(payload));if(accepted)latest=pointer_receiver.latest();}
            if(accepted)(void)enqueue_control(SessionPacketType::Pointer,std::move(latest));
        }
    }

    void send_due(){std::vector<ReliableTransmission>due;{std::lock_guard<std::mutex>lock(reliable_mu);due=reliable_sender.due(monotonic_ms());reliable_pending_count.store(reliable_sender.pending());if(reliable_sender.failed()){set_error("reliable control delivery failed");run.store(false);wake_workers();return;}}for(const auto&tx:due)if(!send_encrypted(SessionPacketType::ReliableControl,tx.sequence,tx.payload)){set_error("peer control send failed");run.store(false);wake_workers();return;}}

    bool enqueue_media(std::span<const std::uint8_t>wire){
        if(!options.media_datagram||wire.empty()||wire.size()>kVideoMaxDatagramBytes+1)return false;
        {
            std::lock_guard<std::mutex>lock(media_mu);
            if(media_count>=media_queue.size()){
                media_queue[media_head].size=0;media_head=(media_head+1)%media_queue.size();--media_count;media_drop_count.fetch_add(1);
            }
            const auto tail=(media_head+media_count)%media_queue.size();auto&slot=media_queue[tail];std::copy(wire.begin(),wire.end(),slot.bytes.begin());slot.size=wire.size();++media_count;
        }
        media_cv.notify_one();return true;
    }

    void media_loop(){std::array<std::uint8_t,kVideoMaxDatagramBytes+1>local{};for(;;){std::size_t size=0;{std::unique_lock<std::mutex>lock(media_mu);media_cv.wait_for(lock,std::chrono::milliseconds(20),[&]{return !run.load()||media_count>0;});if(!run.load()&&media_count==0)break;if(media_count==0)continue;auto&slot=media_queue[media_head];size=slot.size;if(size)std::copy_n(slot.bytes.begin(),size,local.begin());slot.size=0;media_head=(media_head+1)%media_queue.size();--media_count;}if(size&&options.media_datagram)options.media_datagram(std::span<const std::uint8_t>(local.data(),size));}}

    void control_loop(){for(;;){ControlSlot work;bool have=false;{std::unique_lock<std::mutex>lock(control_mu);control_cv.wait_for(lock,std::chrono::milliseconds(20),[&]{return !run.load()||control_count>0;});if(!run.load()&&control_count==0)break;if(control_count==0)continue;auto&slot=control_queue[control_head];work.type=slot.type;work.payload=std::move(slot.payload);slot.payload.clear();control_head=(control_head+1)%control_queue.size();--control_count;have=true;}if(!have)continue;if(work.type==SessionPacketType::ReliableControl){if(options.reliable_input)options.reliable_input(work.payload);}else if(work.type==SessionPacketType::Pointer){if(options.pointer_input)options.pointer_input(work.payload);}}}

    void loop(){auto next_keepalive=Clock::now()+std::chrono::milliseconds(500),last_authenticated=Clock::now();std::array<std::array<std::uint8_t,kVideoMaxDatagramBytes+1>,kUdpReceiveBatchMax>buffers{};std::array<UdpReceiveSlot,kUdpReceiveBatchMax>slots{};for(std::size_t i=0;i<slots.size();++i)slots[i].buffer=buffers[i];while(run.load()){const int count=recv_datagrams_batch(options.socket.fd,slots,10);if(count>0){for(int i=0;i<count&&run.load();++i){auto&s=slots[static_cast<std::size_t>(i)];if(!same_source(s.source,peer)||s.size==0)continue;const std::span<const std::uint8_t>wire(s.buffer.data(),s.size);if(read_magic(wire)!=kSessionPacketMagic)continue;SessionPacketHeader h;std::string payload;if(open_encrypted(wire,&h,&payload)){last_authenticated=Clock::now();process_control(h,std::move(payload));}}for(int i=0;i<count&&run.load();++i){auto&s=slots[static_cast<std::size_t>(i)];if(!same_source(s.source,peer)||s.size==0)continue;const std::span<const std::uint8_t>wire(s.buffer.data(),s.size);if(read_magic(wire)==kSessionPacketMagic)continue;(void)enqueue_media(wire);}}send_due();const auto now=Clock::now();if(!run.load())break;if(now>=next_keepalive){if(!send_encrypted(SessionPacketType::Keepalive,0,"")){set_error("peer keepalive send failed");run.store(false);wake_workers();break;}next_keepalive=now+std::chrono::milliseconds(500);}if(now-last_authenticated>=std::chrono::seconds(3)){set_error("peer control timed out");run.store(false);wake_workers();break;}}established.store(false);wake_workers();}
};

PeerSession::PeerSession():impl_(std::make_unique<Impl>()){}PeerSession::~PeerSession(){stop();}
bool PeerSession::start(PeerSessionOptions options,std::string&error){
    stop();impl_=std::make_unique<Impl>();impl_->options=std::move(options);impl_->session_numeric=numeric_session_id(impl_->options.handshake.session_id);
    if(impl_->options.socket.fd<0||impl_->session_numeric==0||impl_->options.handshake.generation==0||!usable_endpoint(impl_->options.peer)||impl_->options.identity_private_key.empty()){error="invalid peer session options";impl_->set_error(error);return false;}
    bool connected=false;std::string lan_error,direct_error;
    if(impl_->options.lan_peer&&usable_endpoint(*impl_->options.lan_peer)&&!same_endpoint(*impl_->options.lan_peer,impl_->options.peer)){if(impl_->attempt(*impl_->options.lan_peer,false,impl_->options.lan_handshake_timeout_ms,lan_error)){impl_->path="lan";connected=true;}}
    if(!connected){if(impl_->attempt(impl_->options.peer,false,impl_->options.direct_handshake_timeout_ms,direct_error)){impl_->path="direct";connected=true;}else{if(!impl_->options.relay||!retryable_path_error(direct_error)){error=direct_error;if(!lan_error.empty())error="lan: "+lan_error+"; direct: "+direct_error;impl_->set_error(error);return false;}std::string relay_error;if(!impl_->attempt(impl_->options.relay->endpoint,true,impl_->options.relay_handshake_timeout_ms,relay_error)){error="direct: "+direct_error+"; relay: "+relay_error;if(!lan_error.empty())error="lan: "+lan_error+"; "+error;impl_->set_error(error);return false;}impl_->path="relay";connected=true;}}
    impl_->run.store(true);if(impl_->options.reliable_input||impl_->options.pointer_input)impl_->control_thread=std::thread([this]{impl_->control_loop();});if(impl_->options.media_datagram)impl_->media_thread=std::thread([this]{impl_->media_loop();});impl_->thread=std::thread([this]{impl_->loop();});error.clear();return true;
}
bool PeerSession::send_input(std::string command){if(command.rfind("POINTER ",0)==0)return send_pointer(std::move(command));if(!impl_||!impl_->run.load()||command.empty()||command.size()>kReliableControlMaxPayload)return false;std::uint64_t sequence=0;{std::lock_guard<std::mutex>lock(impl_->reliable_mu);sequence=impl_->reliable_sender.enqueue(std::move(command),monotonic_ms());impl_->reliable_pending_count.store(impl_->reliable_sender.pending());}impl_->send_due();return sequence!=0&&impl_->run.load();}
bool PeerSession::send_pointer(std::string command){if(!impl_||!impl_->run.load()||command.rfind("POINTER ",0)!=0||command.size()>kSessionPacketMaxPayload)return false;const auto sequence=impl_->pointer_send_sequence.fetch_add(1)+1;return impl_->send_encrypted(SessionPacketType::Pointer,sequence,command);}
bool PeerSession::send_media_datagram(std::span<const std::uint8_t>wire){return impl_&&impl_->run.load()&&wire.size()<=kRelayMaxInnerBytes&&impl_->send_wire(wire);}
bool PeerSession::established()const{return impl_&&impl_->established.load();}
bool PeerSession::running()const{return impl_&&impl_->run.load();}
std::uint32_t PeerSession::generation()const{return impl_?impl_->options.handshake.generation:0;}
std::uint16_t PeerSession::local_port()const{return impl_?impl_->options.socket.local_port:0;}
std::uint64_t PeerSession::session_id()const{return impl_?impl_->session_numeric:0;}
std::uint64_t PeerSession::reliable_pending()const{return impl_?impl_->reliable_pending_count.load():0;}
std::uint64_t PeerSession::pointer_sequence()const{return impl_?impl_->pointer_send_sequence.load():0;}
std::uint64_t PeerSession::media_ingress_drops()const{return impl_?impl_->media_drop_count.load():0;}
std::uint64_t PeerSession::control_dispatch_drops()const{return impl_?impl_->control_drop_count.load():0;}
std::uint64_t PeerSession::pointer_dispatch_overwrites()const{return impl_?impl_->pointer_overwrite_count.load():0;}
VideoKeys PeerSession::media_keys()const{return impl_?channel_video_keys(impl_->keys.media):VideoKeys{};}
std::string PeerSession::path_name()const{return impl_?impl_->path:"none";}
std::string PeerSession::last_error()const{if(!impl_)return{};std::lock_guard<std::mutex>lock(impl_->state_mu);return impl_->error;}
void PeerSession::stop(){if(!impl_)return;impl_->run.store(false);impl_->wake_workers();if(impl_->thread.joinable())impl_->thread.join();if(impl_->media_thread.joinable())impl_->media_thread.join();if(impl_->control_thread.joinable())impl_->control_thread.join();impl_->clear_media_queue();impl_->clear_control_queue();impl_->established.store(false);close_udp_socket(impl_->options.socket);impl_->cipher.reset();clear_peer_ephemeral(impl_->ephemeral);clear_peer_session_keys(impl_->keys);}

}
