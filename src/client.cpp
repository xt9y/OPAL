#include <opal/client.hpp>
#include <opal/clipboard.hpp>
#include <opal/config.hpp>
#include <opal/crypto.hpp>
#include <opal/input.hpp>
#include <opal/rendezvous_protocol.hpp>
#include <opal/session.hpp>
#include <opal/video_present.hpp>
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>

namespace opal {
namespace {
using Clock=std::chrono::steady_clock;
constexpr std::uint64_t kClipboardReliableWatermark=4;
bool debug_enabled(){const char*v=std::getenv("OPAL_DEBUG");return v&&*v&&std::string(v)!="0";}
bool env_enabled(const char*name){const char*v=std::getenv(name);return v&&*v&&std::string(v)!="0";}
bool read_sdl_clipboard(std::string&text){SDL_ClearError();char*raw=SDL_GetClipboardText();if(!raw)return false;const bool ok=SDL_GetError()[0]=='\0';if(ok)text.assign(raw);SDL_free(raw);return ok;}
void sync_generation(SessionSupervisor&session,HeldInputState&held,unsigned long&generation){auto current=session.control_generation();if(current==generation)return;for(const auto&command:held.release_commands())session.send_input(command);generation=current;}
void release_held(SessionSupervisor&session,HeldInputState&held,unsigned long&generation){sync_generation(session,held,generation);for(const auto&command:held.release_commands())session.send_input(command);}
int sdl_button_to_opal(std::uint8_t button){return button>=1&&button<=3?static_cast<int>(button):0;}
std::string pointer_command_for(SessionSupervisor&session,int x,int y,int width,int height){return video_pointer_command(x,y,width,height,session.remote_width(),session.remote_height());}
bool send_pointer(SessionSupervisor&session,int x,int y,int width,int height){auto command=pointer_command_for(session,x,y,width,height);return command.empty()||session.send_input(command);}
bool send_key_event(SessionSupervisor&session,HeldInputState&held,unsigned long&generation,int scancode,bool down,bool&run,bool&release_capture){
    sync_generation(session,held,generation);const int code=linux_keycode_from_sdl_scancode(scancode);if(code<=0)return true;
    if(down&&held.key_down(code))return true;if(!down&&!held.key_down(code))return true;
    if(down){const auto chord=client_control_chord(held,code);if(chord==ClientControlChord::Quit){run=false;return true;}if(chord==ClientControlChord::ReleaseCapture){release_capture=true;return true;}}
    if(down)held.press_key(code);else held.release_key(code);
    return session.send_input("KEY "+std::to_string(code)+" "+(down?"1":"0"));
}
bool present_frame(SessionSupervisor&session,VideoPresenter&presenter,DecodedVideoFrame&frame){
    if(!frame.frame)return true;const auto pts=frame.pts_us;const auto begin=Clock::now();const bool ok=presenter.present_borrowed({frame.frame,frame.pts_us});const auto end=Clock::now();av_frame_free(&frame.frame);frame.pts_us=0;if(ok){const double ms=std::chrono::duration<double,std::milli>(end-begin).count();session.note_presented_video(pts,ms);}return ok;
}
class ClientClipboardBridge {
public:
    void receive_control(const std::string&line){std::lock_guard<std::mutex>lock(mu_);std::string completed;const auto status=receiver_.receive(line,completed);if(status==ClipboardReceiveStatus::Complete)pending_remote_=std::move(completed);}
    void start(SessionSupervisor&session){generation_=session.control_generation();std::string local;if(read_sdl_clipboard(local)){sender_.prime_local(local);primed_=true;}next_poll_=Clock::now()+std::chrono::milliseconds(100);}
    void pump(SessionSupervisor&session){
        const auto generation=session.control_generation();if(generation!=generation_){std::lock_guard<std::mutex>lock(mu_);receiver_.reset();sender_.restart_transport();generation_=generation;}
        std::optional<std::string>remote;{std::lock_guard<std::mutex>lock(mu_);if(pending_remote_){remote=std::move(pending_remote_);pending_remote_.reset();}}
        if(remote){if(SDL_SetClipboardText(remote->c_str())){sender_.note_remote_applied(*remote);primed_=true;}else{std::lock_guard<std::mutex>lock(mu_);if(!pending_remote_)pending_remote_=std::move(*remote);}}
        const auto now=Clock::now();if(now>=next_poll_){std::string local;if(read_sdl_clipboard(local)){if(primed_)sender_.observe_local(local);else{sender_.prime_local(local);primed_=true;}}next_poll_=now+std::chrono::milliseconds(100);}
        if(session.reliable_pending()>=kClipboardReliableWatermark)return;const auto*message=sender_.next_message();if(message&&session.send_input(*message))sender_.pop_message();
    }
private:
    ClipboardSender sender_;ClipboardReceiver receiver_;std::mutex mu_;std::optional<std::string>pending_remote_;Clock::time_point next_poll_{};unsigned long generation_=0;bool primed_=false;
};
void run_sdl_control(SessionSupervisor&session,VideoPresenter&presenter,ClientClipboardBridge&clipboard){
    HeldInputState held;unsigned long generation=session.control_generation();bool run=true,captured=true;auto size=presenter.window_size();int virtual_x=std::max(0,size.first/2),virtual_y=std::max(0,size.second/2);(void)presenter.set_relative_mouse_mode(true);send_pointer(session,virtual_x,virtual_y,size.first,size.second);
    while(run&&session.running()){
        bool did_work=false;
        SDL_Event event{};
        while(SDL_PollEvent(&event)){
            did_work=true;sync_generation(session,held,generation);bool release_capture=false;
            if(event.type==SDL_EVENT_QUIT||event.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED){run=false;break;}
            if(event.type==SDL_EVENT_WINDOW_FOCUS_LOST){release_held(session,held,generation);captured=false;(void)presenter.set_relative_mouse_mode(false);continue;}
            if(event.type==SDL_EVENT_KEY_DOWN||event.type==SDL_EVENT_KEY_UP){if(event.key.repeat)continue;const bool down=event.type==SDL_EVENT_KEY_DOWN;if(!send_key_event(session,held,generation,static_cast<int>(event.key.scancode),down,run,release_capture)&&!session.running())run=false;if(release_capture){release_held(session,held,generation);captured=false;(void)presenter.set_relative_mouse_mode(false);}continue;}
            size=presenter.window_size();if(size.first<=0||size.second<=0)continue;
            if(event.type==SDL_EVENT_MOUSE_MOTION){if(captured){virtual_x=std::clamp(virtual_x+static_cast<int>(std::lround(event.motion.xrel)),0,size.first-1);virtual_y=std::clamp(virtual_y+static_cast<int>(std::lround(event.motion.yrel)),0,size.second-1);if(!send_pointer(session,virtual_x,virtual_y,size.first,size.second)&&!session.running())run=false;}else{virtual_x=std::clamp(static_cast<int>(std::lround(event.motion.x)),0,size.first-1);virtual_y=std::clamp(static_cast<int>(std::lround(event.motion.y)),0,size.second-1);}continue;}
            if(event.type==SDL_EVENT_MOUSE_BUTTON_DOWN||event.type==SDL_EVENT_MOUSE_BUTTON_UP){const bool down=event.type==SDL_EVENT_MOUSE_BUTTON_DOWN;if(!captured&&down){virtual_x=std::clamp(static_cast<int>(std::lround(event.button.x)),0,size.first-1);virtual_y=std::clamp(static_cast<int>(std::lround(event.button.y)),0,size.second-1);captured=presenter.set_relative_mouse_mode(true);send_pointer(session,virtual_x,virtual_y,size.first,size.second);continue;}if(!captured)continue;const int button=sdl_button_to_opal(event.button.button);if(button){bool ok=send_pointer(session,virtual_x,virtual_y,size.first,size.second);if(down)held.press_button(button);else held.release_button(button);if(ok)ok=session.send_input("BUTTON "+std::to_string(button)+" "+(down?"1":"0"));if(!ok&&!session.running())run=false;}continue;}
            if(event.type==SDL_EVENT_MOUSE_WHEEL&&captured){float y=event.wheel.y;if(event.wheel.direction==SDL_MOUSEWHEEL_FLIPPED)y=-y;const int step=y>0.f?1:(y<0.f?-1:0);if(step&&!session.send_input("WHEEL "+std::to_string(step))&&!session.running())run=false;continue;}
        }
        if(!run||!session.running())break;
        DecodedVideoFrame frame{};if(session.take_latest_video(frame)){did_work=true;if(!present_frame(session,presenter,frame)){std::cerr<<"OPAL presenter failed error="<<SDL_GetError()<<"\n";break;}}
        clipboard.pump(session);
        if(!did_work)SDL_DelayNS(kClientIdleWaitNs);
    }
    release_held(session,held,generation);(void)presenter.set_relative_mouse_mode(false);
}
int error_code_for(const std::string&message){if(message.find("identity")!=std::string::npos)return 3;if(message.find("authentication")!=std::string::npos||message.find("pairing")!=std::string::npos)return 4;if(message.find("rendezvous")!=std::string::npos||message.find("offline")!=std::string::npos)return 2;return 1;}
bool saved_legacy(const Ini&hosts,const std::string&name){return hosts.get(name,"rendezvous_id").empty()&&!hosts.get(name,"address").empty();}
}

int hosts_add(const std::string&name,const std::string&address,const std::string&mac){auto p=Paths::load();ensure_layout(p);std::string id;if(!parse_connection_code(address,id)){std::cerr<<"host must be an OPAL connection code (XXXX-XXXX-XXXX)\n";return 1;}Ini h;h.load(p.hosts);h.set(name,"rendezvous_id",id);h.set(name,"connection_code",format_connection_code(id));h.set(name,"paired","false");if(!mac.empty())h.set(name,"mac",mac);if(!h.save(p.hosts))return 1;std::cout<<"saved host "<<name<<" -> "<<format_connection_code(id)<<"\n";return 0;}
int hosts_list(){auto p=Paths::load();Ini h;if(!h.load(p.hosts)){std::cout<<"No saved hosts.\n";return 0;}for(auto&[s,v]:h.sections())if(!s.empty()){auto id=h.get(s,"rendezvous_id");std::cout<<s<<(id.empty()?"  legacy (re-pair required)":"  "+format_connection_code(id))<<"\n";}return 0;}

int client_connect(const std::string&target_in,const std::string&password_arg,const StreamOptions&stream){
    const bool headless_test=env_enabled("OPAL_TEST_HEADLESS");
    if(!headless_test&&!SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS)){std::cerr<<"SDL3 video initialization failed: "<<SDL_GetError()<<"\n";return 1;}
    auto quit_sdl=[&](){if(!headless_test)SDL_Quit();};
    ClientClipboardBridge clipboard;
    auto p=Paths::load();ensure_layout(p);if(!ensure_identity(p.identity_key,p.identity_pub)){std::cerr<<"client identity generation failed\n";quit_sdl();return 1;}Ini hosts;hosts.load(p.hosts);const bool saved=hosts.sections().count(target_in)>0;std::string rendezvous_id,expected_host_key,tailnet_address;
    if(saved){if(saved_legacy(hosts,target_in)){std::cerr<<"saved host uses an obsolete OPAL host format; remove it and pair again with its current OPAL connection code\n";quit_sdl();return 2;}rendezvous_id=hosts.get(target_in,"rendezvous_id");expected_host_key=hosts.get(target_in,"host_public_key");tailnet_address=hosts.get(target_in,"tailnet_address");}else if(!parse_connection_code(target_in,rendezvous_id)){std::cerr<<"invalid OPAL connection code; expected XXXX-XXXX-XXXX\n";quit_sdl();return 2;}
    if(rendezvous_id.empty()){std::cerr<<"saved OPAL host has no rendezvous identity\n";quit_sdl();return 2;}
    SessionOptions options;options.rendezvous_id=rendezvous_id;options.expected_host_public_key=expected_host_key;options.tailnet_address=tailnet_address;options.client_public_key=public_key_hex(p.identity_pub);options.client_private_key_path=p.identity_key.string();options.paired=saved&&hosts.get(target_in,"paired")=="true"&&!expected_host_key.empty();options.pairing_password=password_arg;if(!headless_test)options.clipboard_control=[&clipboard](const std::string&line){clipboard.receive_control(line);};options.label=saved?target_in:"client";options.stream=stream;
    if(!options.paired&&options.pairing_password.empty())options.pairing_password_provider=[](){std::string password;std::cout<<"Pairing password: "<<std::flush;std::cin>>password;return password;};
    SessionSupervisor session(std::move(options));if(!session.start()){auto message=session.last_error();if(message.empty())message="cannot connect to OPAL host";std::cerr<<message<<"\n";quit_sdl();return error_code_for(message);}
    if(!headless_test)clipboard.start(session);

    const std::string section=saved?target_in:format_connection_code(rendezvous_id);hosts.set(section,"rendezvous_id",rendezvous_id);hosts.set(section,"connection_code",format_connection_code(rendezvous_id));hosts.set(section,"host_public_key",session.host_public_key());hosts.set(section,"paired",session.paired()?"true":"false");auto learned_mac=session.remote_mac();if(!learned_mac.empty())hosts.set(section,"mac",learned_mac);auto learned_tailnet=session.remote_tailnet_address();if(!learned_tailnet.empty())hosts.set(section,"tailnet_address",learned_tailnet);if(hosts.get(section,"mouse_sensitivity").empty())hosts.set(section,"mouse_sensitivity","1.0");hosts.save(p.hosts);

    for(int i=0;i<1000&&!session.media_started()&&session.running();++i){if(!headless_test)clipboard.pump(session);std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    if(!session.media_started()){auto message=session.last_error();std::cerr<<(message.empty()?"direct video did not start":message)<<"\n";session.stop();quit_sdl();return 1;}
    learned_tailnet=session.remote_tailnet_address();if(!learned_tailnet.empty()){hosts.set(section,"tailnet_address",learned_tailnet);hosts.save(p.hosts);}

    if(headless_test){std::cout<<"Connected.\n"<<std::flush;while(session.running())std::this_thread::sleep_for(std::chrono::milliseconds(100));session.stop();return 0;}

    DecodedVideoFrame first{};for(int i=0;i<100&&!first.frame&&session.running();++i){clipboard.pump(session);session.take_latest_video(first);if(!first.frame)std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    if(!first.frame){std::cerr<<"decoded video frame unavailable\n";session.stop();quit_sdl();return 1;}
    bool fullscreen=true;if(const char*w=std::getenv("OPAL_VIDEO_WINDOWED");w&&*w&&std::string(w)!="0")fullscreen=false;
    VideoPresenter presenter;if(!presenter.open(first.frame->width,first.frame->height,fullscreen)){std::cerr<<"OPAL presenter-open failed error="<<SDL_GetError()<<"\n";av_frame_free(&first.frame);session.stop();quit_sdl();return 1;}
    if(debug_enabled())std::cerr<<"OPAL presenter=sdl3 video_driver="<<presenter.backend_name()<<"\n";
    if(!present_frame(session,presenter,first)){std::cerr<<"OPAL presenter failed error="<<SDL_GetError()<<"\n";presenter.close();session.stop();quit_sdl();return 1;}
    std::cout<<"Connected. Ctrl+Alt+Shift+W releases input; click the OPAL screen to capture again. Ctrl+Alt+Shift+Q quits.\n"<<std::flush;if(debug_enabled())std::cerr<<"OPAL connection path="<<session.path_name()<<"\n";
    run_sdl_control(session,presenter,clipboard);presenter.close();session.stop();quit_sdl();return 0;
}
int client_connect(const std::string&target_in,const std::string&password_arg){return client_connect(target_in,password_arg,{});}
}
