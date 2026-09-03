#include <opal/session.hpp>
#include <opal/crypto.hpp>
#include <opal/peer_session.hpp>
#include <opal/rendezvous_client.hpp>
#include <opal/video_receiver.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

namespace opal { namespace {
using namespace std::chrono_literals;
bool debug_enabled(){const char*v=std::getenv("OPAL_DEBUG");return v&&*v&&std::string(v)!="0";}
bool pointer_command(const std::string&s){return s.rfind("POINTER ",0)==0;}
}

struct SessionSupervisor::Impl {
    explicit Impl(SessionOptions o):options(std::move(o)),paired_state(options.paired){}
    SessionOptions options;std::atomic<bool>run{false},paired_state{false};std::atomic<unsigned long>generation{0};
    mutable std::mutex session_mu,state_mu;std::unique_ptr<PeerSession>peer;std::unique_ptr<VideoReceiver>receiver;std::thread monitor_thread;
    std::string host_key_value,path_value,error;int remote_width_value=0,remote_height_value=0;std::string remote_mac_value;

    void set_error(std::string text){std::lock_guard<std::mutex>lock(state_mu);error=std::move(text);}
    void parse_host_meta(const std::string&line){std::istringstream in(line);std::string word,mac,extra;int width=0,height=0;if(!(in>>word>>width>>height>>mac)||in>>extra||word!="HOST_META"||width<=0||height<=0)return;std::lock_guard<std::mutex>lock(state_mu);remote_width_value=width;remote_height_value=height;remote_mac_value=mac=="-"?"":mac;}
    void teardown_current(){std::unique_ptr<VideoReceiver>old_receiver;std::unique_ptr<PeerSession>old_peer;{std::lock_guard<std::mutex>lock(session_mu);old_receiver=std::move(receiver);old_peer=std::move(peer);}if(old_receiver)old_receiver->stop();if(old_peer)old_peer->stop();}

    bool connect_generation(std::uint32_t gen,bool allow_pair){
        RendezvousClient rendezvous;if(!rendezvous.open()){set_error("cannot reach OPAL rendezvous service");return false;}
        RendezvousIntroduction intro;std::string connect_error;if(!rendezvous.introduce(options.rendezvous_id,options.client_public_key,intro,connect_error)){set_error(connect_error.empty()?"OPAL host is offline":connect_error);return false;}
        std::string expected;{std::lock_guard<std::mutex>lock(state_mu);expected=!options.expected_host_public_key.empty()?options.expected_host_public_key:host_key_value;}
        if(paired_state.load()&&expected.empty()){set_error("saved host identity unavailable");return false;}if(!expected.empty()&&!secure_equal(expected,intro.peer_public_key)){set_error("host identity changed; refusing connection");return false;}
        const bool pairing=!paired_state.load();if(pairing&&!allow_pair){set_error("paired recovery cannot fall back to pairing");return false;}std::string password=options.pairing_password;if(pairing&&password.empty()&&options.pairing_password_provider)password=options.pairing_password_provider();if(pairing){password=normalize_pairing_code(password);if(password.empty()){set_error("pairing password required");return false;}}
        RelayAllocation relay;std::string relay_error;const bool relay_ready=rendezvous.request_relay(intro.session_id,options.client_public_key,options.client_private_key_path,relay,relay_error);
        auto socket=rendezvous.take_socket();if(socket.fd<0){set_error("rendezvous did not preserve peer socket");return false;}
        auto next_receiver=std::make_unique<VideoReceiver>();auto next_peer=std::make_unique<PeerSession>();PeerSession*peer_ptr=next_peer.get();VideoReceiver*receiver_ptr=next_receiver.get();
        PeerSessionOptions peer_options;peer_options.client_side=true;peer_options.socket=socket;peer_options.peer=intro.peer_observed;if(!intro.peer_local.host.empty()&&intro.peer_local.port)peer_options.lan_peer=intro.peer_local;if(relay_ready)peer_options.relay=PeerRelayFallback{relay.endpoint,relay.allocation_id,RelayRole::Client};peer_options.handshake.rendezvous_id=intro.rendezvous_id;peer_options.handshake.session_id=intro.session_id;peer_options.handshake.generation=1;peer_options.handshake.client_identity=options.client_public_key;peer_options.handshake.host_identity=intro.peer_public_key;peer_options.handshake.client_nonce=intro.local_nonce;peer_options.handshake.host_nonce=intro.peer_nonce;peer_options.handshake.auth_binding=pairing?"pairing":"paired";peer_options.identity_private_key=options.client_private_key_path;peer_options.pairing_password=password;
        peer_options.reliable_input=[this,receiver_ptr](const std::string&line){if(line.rfind("HOST_META ",0)==0){parse_host_meta(line);return;}if(line.rfind("MEDIA_ERROR ",0)==0){set_error(line.substr(12));return;}receiver_ptr->handle_control_line(line);};
        peer_options.media_datagram=[receiver_ptr](std::span<const std::uint8_t>wire){receiver_ptr->accept_datagram(wire);};
        std::string peer_error;if(!next_peer->start(std::move(peer_options),peer_error)){set_error(peer_error.empty()?"direct peer session failed":peer_error);return false;}
        if(!next_receiver->start_native(next_peer->media_keys(),next_peer->session_id(),gen,[peer_ptr](const std::string&line){return peer_ptr->send_input(line);})){
            next_peer->stop();set_error("could not start native video receiver");return false;
        }
        const int debug=debug_enabled()?1:0;const std::string ready="MEDIA_RECEIVER_READY "+std::to_string(gen)+" "+std::to_string(options.stream.max_width)+" "+std::to_string(options.stream.max_height)+" "+std::to_string(options.stream.fps)+" "+std::to_string(debug);if(!next_peer->send_input(ready)){next_receiver->stop();next_peer->stop();set_error("could not announce media receiver readiness");return false;}
        {std::lock_guard<std::mutex>lock(state_mu);host_key_value=intro.peer_public_key;options.expected_host_public_key=intro.peer_public_key;path_value=next_peer->path_name();error.clear();}
        paired_state.store(true);generation.store(gen);{std::lock_guard<std::mutex>lock(session_mu);peer=std::move(next_peer);receiver=std::move(next_receiver);}if(debug_enabled())std::cerr<<"OPAL network path="<<path_value<<" rendezvous="<<intro.rendezvous_id<<" media_generation="<<gen<<"\n";return true;
    }

    bool current_healthy(){std::lock_guard<std::mutex>lock(session_mu);return peer&&peer->running()&&receiver&&!receiver->failed();}
    void monitor(){while(run.load()){std::this_thread::sleep_for(100ms);if(!run.load())break;if(current_healthy())continue;const auto failed_generation=static_cast<std::uint32_t>(generation.load());teardown_current();bool restored=false;for(int attempt=0;attempt<5&&run.load();++attempt){const auto next=failed_generation+1;if(connect_generation(next,false)){restored=true;if(debug_enabled())std::cerr<<"OPAL peer session recovered media_generation="<<next<<" path="<<path_value<<"\n";break;}std::this_thread::sleep_for(std::chrono::milliseconds(250*(1<<std::min(attempt,4))));}if(!restored){run.store(false);break;}}}

    bool start(){if(run.load())return true;if(options.rendezvous_id.empty()||options.client_public_key.empty()||options.client_private_key_path.empty()){set_error("invalid native session configuration");return false;}run.store(true);if(!connect_generation(1,true)){run.store(false);teardown_current();return false;}monitor_thread=std::thread([this]{monitor();});return true;}
    void stop(){const bool was=run.exchange(false);if(monitor_thread.joinable())monitor_thread.join();teardown_current();if(!was)return;}
    bool send_input(const std::string&command){if(command.empty()||!run.load())return false;std::lock_guard<std::mutex>lock(session_mu);if(!peer||!peer->running())return false;return pointer_command(command)?peer->send_pointer(command):peer->send_input(command);}
    bool media_started()const{std::lock_guard<std::mutex>lock(session_mu);return receiver&&receiver->media_started();}
};

SessionSupervisor::SessionSupervisor(SessionOptions o):impl_(std::make_unique<Impl>(std::move(o))){}SessionSupervisor::~SessionSupervisor(){impl_->stop();}
bool SessionSupervisor::start(){return impl_->start();}void SessionSupervisor::stop(){impl_->stop();}bool SessionSupervisor::send_input(const std::string&c){return impl_->send_input(c);}unsigned long SessionSupervisor::control_generation()const{return impl_->generation.load();}bool SessionSupervisor::media_started()const{return impl_->media_started();}bool SessionSupervisor::running()const{return impl_->run.load();}bool SessionSupervisor::paired()const{return impl_->paired_state.load();}
int SessionSupervisor::remote_width()const{std::lock_guard<std::mutex>lock(impl_->state_mu);return impl_->remote_width_value;}int SessionSupervisor::remote_height()const{std::lock_guard<std::mutex>lock(impl_->state_mu);return impl_->remote_height_value;}std::string SessionSupervisor::remote_mac()const{std::lock_guard<std::mutex>lock(impl_->state_mu);return impl_->remote_mac_value;}std::string SessionSupervisor::host_public_key()const{std::lock_guard<std::mutex>lock(impl_->state_mu);return impl_->host_key_value;}std::string SessionSupervisor::fingerprint()const{return host_public_key();}std::string SessionSupervisor::path_name()const{std::lock_guard<std::mutex>lock(impl_->state_mu);return impl_->path_value;}std::string SessionSupervisor::last_error()const{std::lock_guard<std::mutex>lock(impl_->state_mu);return impl_->error;}

}
