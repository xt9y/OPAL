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

bool release_chord(const HeldInputState&held,int code){
    bool ctrl=held.key_down(KEY_LEFTCTRL)||held.key_down(KEY_RIGHTCTRL);
    bool alt=held.key_down(KEY_LEFTALT)||held.key_down(KEY_RIGHTALT);
    bool shift=held.key_down(KEY_LEFTSHIFT)||held.key_down(KEY_RIGHTSHIFT);
    return code==KEY_Q&&ctrl&&alt&&shift;
}

void sync_generation(SessionSupervisor&session,HeldInputState&held,unsigned long&generation){
    auto current=session.control_generation();
    if(current==generation)return;
    // The host releases every key/button held by a dead control generation.
    // Drop matching local assumptions rather than carrying them into the new one.
    held.release_commands();
    generation=current;
}

bool setup_xinput2(Display*d,Window root,int&opcode){
    int event=0,error=0;
    if(!XQueryExtension(d,"XInputExtension",&opcode,&event,&error))return false;
    int major=2,minor=0;
    if(XIQueryVersion(d,&major,&minor)!=Success||major<2)return false;
    unsigned char mask[XIMaskLen(XI_LASTEVENT)]{};
    XISetMask(mask,XI_RawKeyPress);XISetMask(mask,XI_RawKeyRelease);
    XISetMask(mask,XI_RawButtonPress);XISetMask(mask,XI_RawButtonRelease);XISetMask(mask,XI_RawMotion);
    XIEventMask selection{XIAllMasterDevices,static_cast<int>(sizeof(mask)),mask};
    if(XISelectEvents(d,root,&selection,1)!=Success)return false;
    XFlush(d);return true;
}

bool raw_motion_values(const XIRawEvent*raw,double&dx,double&dy){
    dx=0.0;dy=0.0;if(!raw||!raw->valuators.mask||!raw->raw_values)return false;
    int value_index=0;
    for(int axis=0;axis<raw->valuators.mask_len*8;axis++){
        if(!XIMaskIsSet(raw->valuators.mask,axis))continue;
        double value=raw->raw_values[value_index++];
        if(axis==0)dx=value;else if(axis==1)dy=value;
    }
    return dx!=0.0||dy!=0.0;
}

bool send_key_event(SessionSupervisor&session,HeldInputState&held,unsigned long&generation,unsigned int keycode,bool down,bool&run){
    sync_generation(session,held,generation);
    int code=linux_keycode_from_x11(keycode);
    if(code<=0)return true;
    if(down&&release_chord(held,code)){run=false;return true;}
    if(down)held.press_key(code);else held.release_key(code);
    return session.send_input("KEY "+std::to_string(code)+" "+(down?"1":"0"));
}

void release_held(SessionSupervisor&session,HeldInputState&held,unsigned long&generation){
    sync_generation(session,held,generation);
    for(const auto&command:held.release_commands())session.send_input(command);
}

void run_xinput2_control(Display*d,Window root,int opcode,SessionSupervisor&session,double sensitivity){
    HeldInputState held;
    unsigned long generation=session.control_generation();
    int keyboard_grab=XGrabKeyboard(d,root,True,GrabModeAsync,GrabModeAsync,CurrentTime);
    XGrabPointer(d,root,True,0,GrabModeAsync,GrabModeAsync,None,None,CurrentTime);
    XFlush(d);
    bool run=true;
    while(run&&session.running()){
        XEvent event;XNextEvent(d,&event);
        sync_generation(session,held,generation);
        if(event.type==KeyPress||event.type==KeyRelease){
            bool down=event.type==KeyPress;
            if(!send_key_event(session,held,generation,event.xkey.keycode,down,run)&&!session.running())run=false;
            continue;
        }
        if(event.xcookie.type!=GenericEvent||event.xcookie.extension!=opcode)continue;
        if(!XGetEventData(d,&event.xcookie))continue;
        auto*raw=static_cast<XIRawEvent*>(event.xcookie.data);bool send_ok=true;
        switch(event.xcookie.evtype){
            case XI_RawKeyPress:
            case XI_RawKeyRelease:
                if(keyboard_grab!=GrabSuccess)send_ok=send_key_event(session,held,generation,static_cast<unsigned int>(raw->detail),event.xcookie.evtype==XI_RawKeyPress,run);
                break;
            case XI_RawButtonPress:
            case XI_RawButtonRelease:{
                int button=raw->detail;bool down=event.xcookie.evtype==XI_RawButtonPress;
                if((button==4||button==5)&&down)send_ok=session.send_input("WHEEL "+std::to_string(button==4?1:-1));
                else if(button>=1&&button<=3){if(down)held.press_button(button);else held.release_button(button);send_ok=session.send_input("BUTTON "+std::to_string(button)+" "+(down?"1":"0"));}
                break;
            }
            case XI_RawMotion:{
                double dx=0.0,dy=0.0;
                if(raw_motion_values(raw,dx,dy)){
                    auto command=normalized_motion_command(dx,dy,0,0,sensitivity);
                    if(!command.empty())send_ok=session.send_input(command);
                }
                break;
            }
            default:break;
        }
        XFreeEventData(d,&event.xcookie);
        if(!send_ok&&!session.running())run=false;
    }
    release_held(session,held,generation);
    if(keyboard_grab==GrabSuccess)XUngrabKeyboard(d,CurrentTime);
    XUngrabPointer(d,CurrentTime);XFlush(d);
}

void run_x11_fallback_control(Display*d,Window root,SessionSupervisor&session,double sensitivity){
    XWindowAttributes wa{};XGetWindowAttributes(d,root,&wa);int cx=wa.width/2,cy=wa.height/2;
    HeldInputState held;unsigned long generation=session.control_generation();
    XGrabKeyboard(d,root,True,GrabModeAsync,GrabModeAsync,CurrentTime);
    XGrabPointer(d,root,True,PointerMotionMask|ButtonPressMask|ButtonReleaseMask,GrabModeAsync,GrabModeAsync,None,None,CurrentTime);
    XWarpPointer(d,None,root,0,0,0,0,cx,cy);XFlush(d);
    bool run=true;
    while(run&&session.running()){
        XEvent e;XNextEvent(d,&e);sync_generation(session,held,generation);
        if(e.type==KeyPress||e.type==KeyRelease){
            auto*k=&e.xkey;KeySym sym=XLookupKeysym(k,0);
            if(e.type==KeyPress&&sym==XK_q&&(k->state&ControlMask)&&(k->state&Mod1Mask)&&(k->state&ShiftMask)){run=false;break;}
            int code=linux_keycode_from_x11(k->keycode);bool down=e.type==KeyPress;
            if(code>0){if(down)held.press_key(code);else held.release_key(code);if(!session.send_input("KEY "+std::to_string(code)+" "+(down?"1":"0"))&&!session.running()){run=false;break;}}
        }else if(e.type==MotionNotify){
            int dx=e.xmotion.x_root-cx,dy=e.xmotion.y_root-cy;
            if(dx||dy){auto command=normalized_motion_command(dx,dy,0,0,sensitivity);if(!command.empty()&&!session.send_input(command)&&!session.running()){run=false;break;}XWarpPointer(d,None,root,0,0,0,0,cx,cy);XFlush(d);}
        }else if(e.type==ButtonPress||e.type==ButtonRelease){
            int b=e.xbutton.button;bool down=e.type==ButtonPress,ok=true;
            if((b==4||b==5)&&down)ok=session.send_input("WHEEL "+std::to_string(b==4?1:-1));
            else if(b<=3){if(down)held.press_button(b);else held.release_button(b);ok=session.send_input("BUTTON "+std::to_string(b)+" "+(down?"1":"0"));}
            if(!ok&&!session.running()){run=false;break;}
        }
    }
    release_held(session,held,generation);
    XUngrabKeyboard(d,CurrentTime);XUngrabPointer(d,CurrentTime);XFlush(d);
}

double saved_mouse_sensitivity(const Ini&hosts,const std::string&name,bool saved){
    if(!saved)return 1.0;
    try{return clamp_mouse_sensitivity(std::stod(hosts.get(name,"mouse_sensitivity","1.0")));}
    catch(...){return 1.0;}
}

int error_code_for(const std::string&message){
    if(message.find("tunnel")!=std::string::npos)return 2;
    if(message.find("certificate")!=std::string::npos)return 3;
    if(message.find("authentication")!=std::string::npos||message.find("pairing")!=std::string::npos)return 4;
    return 1;
}
}

int hosts_add(const std::string&name,const std::string&address,const std::string&mac){auto p=Paths::load();ensure_layout(p);Ini h;h.load(p.hosts);h.set(name,"address",address);h.set(name,"port","47990");h.set(name,"video_port","47991");if(!mac.empty())h.set(name,"mac",mac);if(!h.save(p.hosts))return 1;std::cout<<"saved host "<<name<<" -> "<<address<<"\n";return 0;}
int hosts_list(){auto p=Paths::load();Ini h;if(!h.load(p.hosts)){std::cout<<"No saved hosts.\n";return 0;}for(auto&[s,v]:h.sections())if(!s.empty()){auto it=v.find("address");std::cout<<s<<(it==v.end()?"":"  "+it->second)<<"\n";}return 0;}

int client_connect(const std::string&target_in,const std::string&password_arg){
    auto p=Paths::load();ensure_layout(p);ensure_identity(p.identity_key,p.identity_pub);
    Ini hosts;hosts.load(p.hosts);
    std::string target=target_in,saved_address;
    int cp=47990,vp=47991;
    bool saved=hosts.sections().count(target_in)>0,tunneled=false;
    if(saved){target=hosts.get(target_in,"address");saved_address=target;cp=hosts.get_int(target_in,"port",47990);vp=hosts.get_int(target_in,"video_port",47991);}
    double sensitivity=saved_mouse_sensitivity(hosts,target_in,saved);

    std::string control_token,video_token;
    if(tunnel_connection_code(target,&control_token,&video_token)){
        tunneled=true;target="127.0.0.1";cp=47990;vp=47991;
    }else if(target.rfind("zrok:",0)==0){
        auto tokens=target.substr(5);auto comma=tokens.find(',');
        if(comma==std::string::npos||comma==0||comma+1>=tokens.size()){
            std::cerr<<"invalid zrok connection code\n";return 2;
        }
        control_token=tokens.substr(0,comma);video_token=tokens.substr(comma+1);
        tunneled=true;target="127.0.0.1";cp=47990;vp=47991;
    }

    SessionOptions options;
    options.target=target;options.control_port=cp;options.video_port=vp;
    options.tunneled=tunneled;options.control_token=control_token;options.video_token=video_token;
    options.fingerprint=saved?hosts.get(target_in,"fingerprint"):"";
    options.client_public_key=public_key_hex(p.identity_pub);
    options.client_private_key_path=p.identity_key.string();
    options.paired=saved&&hosts.get(target_in,"paired")=="true";
    options.pairing_password=password_arg;
    options.label=target_in;
    if(!options.paired&&options.pairing_password.empty()){
        options.pairing_password_provider=[](){std::string password;std::cout<<"Pairing password: "<<std::flush;std::cin>>password;return password;};
    }

    SessionSupervisor session(std::move(options));
    if(!session.start()){
        auto message=session.last_error();
        if(message.empty())message="cannot connect to OPAL host";
        std::cerr<<message<<"\n";
        return error_code_for(message);
    }

    if(!saved){
        hosts.set(target_in,"address",saved_address.empty()?target_in:saved_address);
        hosts.set(target_in,"port",std::to_string(cp));
        hosts.set(target_in,"video_port",std::to_string(vp));
    }
    hosts.set(target_in,"fingerprint",session.fingerprint());
    hosts.set(target_in,"paired",session.paired()?"true":"false");
    if(hosts.get(target_in,"mouse_sensitivity").empty())hosts.set(target_in,"mouse_sensitivity","1.0");
    hosts.save(p.hosts);

    for(int i=0;i<100&&!session.media_started()&&session.running();++i)std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if(!session.media_started()){
        std::cerr<<"video did not start\n";
        session.stop();return 1;
    }
    std::cout<<"Connected. Ctrl+Alt+Shift+Q releases remote control.\n";

    Display*d=XOpenDisplay(nullptr);
    if(!d){
        std::cerr<<"DISPLAY unavailable; video only\n";
        while(session.running())std::this_thread::sleep_for(std::chrono::milliseconds(500));
        session.stop();return 0;
    }

    Window root=DefaultRootWindow(d);int xi_opcode=0;
    if(setup_xinput2(d,root,xi_opcode))run_xinput2_control(d,root,xi_opcode,session,sensitivity);
    else run_x11_fallback_control(d,root,session,sensitivity);
    XCloseDisplay(d);session.stop();return 0;
}
}
