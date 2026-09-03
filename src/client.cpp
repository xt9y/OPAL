#include <opal/client.hpp>
#include <opal/config.hpp>
#include <opal/crypto.hpp>
#include <opal/input.hpp>
#include <opal/session.hpp>
#include <opal/tunnel.hpp>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XInput2.h>
#include <linux/input-event-codes.h>
#include <chrono>
#include <iostream>
#include <thread>

namespace opal {
namespace {

bool release_chord(const HeldInputState&held,int code){bool ctrl=held.key_down(KEY_LEFTCTRL)||held.key_down(KEY_RIGHTCTRL);bool alt=held.key_down(KEY_LEFTALT)||held.key_down(KEY_RIGHTALT);bool shift=held.key_down(KEY_LEFTSHIFT)||held.key_down(KEY_RIGHTSHIFT);return code==KEY_Q&&ctrl&&alt&&shift;}
void sync_generation(SessionSupervisor&session,HeldInputState&held,unsigned long&generation){auto current=session.control_generation();if(current==generation)return;held.release_commands();generation=current;}
bool setup_xinput2(Display*d,Window root,int&opcode){int event=0,error=0;if(!XQueryExtension(d,"XInputExtension",&opcode,&event,&error))return false;int major=2,minor=0;if(XIQueryVersion(d,&major,&minor)!=Success||major<2)return false;unsigned char mask[XIMaskLen(XI_LASTEVENT)]{};XISetMask(mask,XI_RawKeyPress);XISetMask(mask,XI_RawKeyRelease);XISetMask(mask,XI_RawButtonPress);XISetMask(mask,XI_RawButtonRelease);XIEventMask selection{XIAllMasterDevices,static_cast<int>(sizeof(mask)),mask};if(XISelectEvents(d,root,&selection,1)!=Success)return false;XFlush(d);return true;}
std::string pointer_command_for(SessionSupervisor&session,int x,int y,int width,int height){return video_pointer_command(x,y,width,height,session.remote_width(),session.remote_height());}
bool send_pointer_position(Display*d,Window root,int width,int height,SessionSupervisor&session){Window returned_root=None,child=None;int root_x=0,root_y=0,win_x=0,win_y=0;unsigned int mask=0;if(!XQueryPointer(d,root,&returned_root,&child,&root_x,&root_y,&win_x,&win_y,&mask))return true;auto command=pointer_command_for(session,root_x,root_y,width,height);return command.empty()||session.send_input(command);}
bool send_pointer_position(int x,int y,int width,int height,SessionSupervisor&session){auto command=pointer_command_for(session,x,y,width,height);return command.empty()||session.send_input(command);}
bool send_key_event(SessionSupervisor&session,HeldInputState&held,unsigned long&generation,unsigned int keycode,bool down,bool&run){sync_generation(session,held,generation);int code=linux_keycode_from_x11(keycode);if(code<=0)return true;if(down&&release_chord(held,code)){run=false;return true;}if(down)held.press_key(code);else held.release_key(code);return session.send_input("KEY "+std::to_string(code)+" "+(down?"1":"0"));}
void release_held(SessionSupervisor&session,HeldInputState&held,unsigned long&generation){sync_generation(session,held,generation);for(const auto&command:held.release_commands())session.send_input(command);}

void run_xinput2_control(Display*d,Window root,int opcode,SessionSupervisor&session){
    XWindowAttributes wa{};if(!XGetWindowAttributes(d,root,&wa)||wa.width<=0||wa.height<=0)return;HeldInputState held;unsigned long generation=session.control_generation();int keyboard_grab=XGrabKeyboard(d,root,True,GrabModeAsync,GrabModeAsync,CurrentTime);XGrabPointer(d,root,True,PointerMotionMask,GrabModeAsync,GrabModeAsync,None,None,CurrentTime);XFlush(d);send_pointer_position(d,root,wa.width,wa.height,session);bool run=true;
    while(run&&session.running()){XEvent event;XNextEvent(d,&event);sync_generation(session,held,generation);if(event.type==KeyPress||event.type==KeyRelease){bool down=event.type==KeyPress;if(!send_key_event(session,held,generation,event.xkey.keycode,down,run)&&!session.running())run=false;continue;}if(event.type==MotionNotify){if(!send_pointer_position(event.xmotion.x_root,event.xmotion.y_root,wa.width,wa.height,session)&&!session.running())run=false;continue;}if(event.xcookie.type!=GenericEvent||event.xcookie.extension!=opcode)continue;if(!XGetEventData(d,&event.xcookie))continue;auto*raw=static_cast<XIRawEvent*>(event.xcookie.data);bool send_ok=true;switch(event.xcookie.evtype){case XI_RawKeyPress:case XI_RawKeyRelease:if(keyboard_grab!=GrabSuccess)send_ok=send_key_event(session,held,generation,static_cast<unsigned int>(raw->detail),event.xcookie.evtype==XI_RawKeyPress,run);break;case XI_RawButtonPress:case XI_RawButtonRelease:{send_ok=send_pointer_position(d,root,wa.width,wa.height,session);int button=raw->detail;bool down=event.xcookie.evtype==XI_RawButtonPress;if(send_ok&&(button==4||button==5)&&down)send_ok=session.send_input("WHEEL "+std::to_string(button==4?1:-1));else if(send_ok&&button>=1&&button<=3){if(down)held.press_button(button);else held.release_button(button);send_ok=session.send_input("BUTTON "+std::to_string(button)+" "+(down?"1":"0"));}break;}default:break;}XFreeEventData(d,&event.xcookie);if(!send_ok&&!session.running())run=false;}
    release_held(session,held,generation);if(keyboard_grab==GrabSuccess)XUngrabKeyboard(d,CurrentTime);XUngrabPointer(d,CurrentTime);XFlush(d);
}

void run_x11_fallback_control(Display*d,Window root,SessionSupervisor&session){
    XWindowAttributes wa{};if(!XGetWindowAttributes(d,root,&wa)||wa.width<=0||wa.height<=0)return;HeldInputState held;unsigned long generation=session.control_generation();XGrabKeyboard(d,root,True,GrabModeAsync,GrabModeAsync,CurrentTime);XGrabPointer(d,root,True,PointerMotionMask|ButtonPressMask|ButtonReleaseMask,GrabModeAsync,GrabModeAsync,None,None,CurrentTime);XFlush(d);send_pointer_position(d,root,wa.width,wa.height,session);bool run=true;
    while(run&&session.running()){XEvent e;XNextEvent(d,&e);sync_generation(session,held,generation);if(e.type==KeyPress||e.type==KeyRelease){auto*k=&e.xkey;KeySym sym=XLookupKeysym(k,0);if(e.type==KeyPress&&sym==XK_q&&(k->state&ControlMask)&&(k->state&Mod1Mask)&&(k->state&ShiftMask)){run=false;break;}int code=linux_keycode_from_x11(k->keycode);bool down=e.type==KeyPress;if(code>0){if(down)held.press_key(code);else held.release_key(code);if(!session.send_input("KEY "+std::to_string(code)+" "+(down?"1":"0"))&&!session.running()){run=false;break;}}}else if(e.type==MotionNotify){if(!send_pointer_position(e.xmotion.x_root,e.xmotion.y_root,wa.width,wa.height,session)&&!session.running()){run=false;break;}}else if(e.type==ButtonPress||e.type==ButtonRelease){int b=e.xbutton.button;bool down=e.type==ButtonPress;bool ok=send_pointer_position(e.xbutton.x_root,e.xbutton.y_root,wa.width,wa.height,session);if(ok&&(b==4||b==5)&&down)ok=session.send_input("WHEEL "+std::to_string(b==4?1:-1));else if(ok&&b<=3){if(down)held.press_button(b);else held.release_button(b);ok=session.send_input("BUTTON "+std::to_string(b)+" "+(down?"1":"0"));}if(!ok&&!session.running()){run=false;break;}}}
    release_held(session,held,generation);XUngrabKeyboard(d,CurrentTime);XUngrabPointer(d,CurrentTime);XFlush(d);
}

int error_code_for(const std::string&message){if(message.find("tunnel")!=std::string::npos)return 2;if(message.find("certificate")!=std::string::npos)return 3;if(message.find("authentication")!=std::string::npos||message.find("pairing")!=std::string::npos)return 4;return 1;}
}

int hosts_add(const std::string&name,const std::string&address,const std::string&mac){auto p=Paths::load();ensure_layout(p);Ini h;h.load(p.hosts);h.set(name,"address",address);h.set(name,"port","47990");if(!mac.empty())h.set(name,"mac",mac);if(!h.save(p.hosts))return 1;std::cout<<"saved host "<<name<<" -> "<<address<<"\n";return 0;}
int hosts_list(){auto p=Paths::load();Ini h;if(!h.load(p.hosts)){std::cout<<"No saved hosts.\n";return 0;}for(auto&[s,v]:h.sections())if(!s.empty()){auto it=v.find("address");std::cout<<s<<(it==v.end()?"":"  "+it->second)<<"\n";}return 0;}

int client_connect(const std::string&target_in,const std::string&password_arg,const StreamOptions&stream){
    auto p=Paths::load();ensure_layout(p);ensure_identity(p.identity_key,p.identity_pub);Ini hosts;hosts.load(p.hosts);std::string target=target_in,saved_address;int cp=47990;bool saved=hosts.sections().count(target_in)>0,tunneled=false;
    if(saved){target=hosts.get(target_in,"address");saved_address=target;cp=hosts.get_int(target_in,"port",47990);}
    std::string control_token,legacy_video_token;
    if(tunnel_connection_code(target,&control_token,&legacy_video_token)){tunneled=true;target="127.0.0.1";cp=47990;}
    else if(target.rfind("zrok:",0)==0){auto body=target.substr(5);auto comma=body.find(',');control_token=comma==std::string::npos?body:body.substr(0,comma);if(control_token.empty()){std::cerr<<"invalid zrok control code\n";return 2;}tunneled=true;target="127.0.0.1";cp=47990;}

    SessionOptions options;options.target=target;options.control_port=cp;options.tunneled=tunneled;options.control_token=control_token;options.fingerprint=saved?hosts.get(target_in,"fingerprint"):"";options.client_public_key=public_key_hex(p.identity_pub);options.client_private_key_path=p.identity_key.string();options.paired=saved&&hosts.get(target_in,"paired")=="true";options.pairing_password=password_arg;options.label=target_in;options.stream=stream;
    if(!options.paired&&options.pairing_password.empty())options.pairing_password_provider=[](){std::string password;std::cout<<"Pairing password: "<<std::flush;std::cin>>password;return password;};
    SessionSupervisor session(std::move(options));if(!session.start()){auto message=session.last_error();if(message.empty())message="cannot connect to OPAL host";std::cerr<<message<<"\n";return error_code_for(message);}

    if(!saved){hosts.set(target_in,"address",saved_address.empty()?target_in:saved_address);hosts.set(target_in,"port",std::to_string(cp));}
    hosts.set(target_in,"video_port","");hosts.set(target_in,"fingerprint",session.fingerprint());hosts.set(target_in,"paired",session.paired()?"true":"false");auto learned_mac=session.remote_mac();if(!learned_mac.empty())hosts.set(target_in,"mac",learned_mac);if(hosts.get(target_in,"mouse_sensitivity").empty())hosts.set(target_in,"mouse_sensitivity","1.0");hosts.save(p.hosts);

    for(int i=0;i<100&&!session.media_started()&&session.running();++i)std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if(!session.media_started()){auto message=session.last_error();std::cerr<<(message.empty()?"direct video did not start":message)<<"\n";session.stop();return 1;}
    std::cout<<"Connected. Ctrl+Alt+Shift+Q releases remote control.\n";
    Display*d=XOpenDisplay(nullptr);if(!d){std::cerr<<"DISPLAY unavailable; direct video cannot accept local input\n";while(session.running())std::this_thread::sleep_for(std::chrono::milliseconds(500));session.stop();return 0;}
    Window root=DefaultRootWindow(d);int xi_opcode=0;if(setup_xinput2(d,root,xi_opcode))run_xinput2_control(d,root,xi_opcode,session);else run_x11_fallback_control(d,root,session);XCloseDisplay(d);session.stop();return 0;
}
int client_connect(const std::string&target_in,const std::string&password_arg){return client_connect(target_in,password_arg,{});}
}
