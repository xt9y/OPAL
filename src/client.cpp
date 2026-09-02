#include <opal/client.hpp>
#include <opal/config.hpp>
#include <opal/crypto.hpp>
#include <opal/input.hpp>
#include <opal/net.hpp>
#include <opal/tunnel.hpp>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XInput2.h>
#include <linux/input-event-codes.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace opal {
static bool send_control(SSL*s,const std::string&x){return tls_write_line(s,x);}
static bool debug_enabled(){const char*v=std::getenv("OPAL_DEBUG");if(!v||!*v)return false;std::string x=v;return x!="0"&&x!="false"&&x!="FALSE"&&x!="off"&&x!="OFF";}

static void video_player(SSL*ssl){
    const char*e=std::getenv("OPAL_PLAYER_CMD");
    const char*cmd=(e&&*e)?e:(debug_enabled()?"ffplay -hide_banner -loglevel warning -fflags nobuffer -flags low_delay -framedrop -fs -autoexit -i pipe:0":"ffplay -hide_banner -loglevel quiet -fflags nobuffer -flags low_delay -framedrop -fs -autoexit -i pipe:0 >/dev/null 2>&1");
    FILE*p=popen(cmd,"w");
    if(!p)return;
    char buf[65536];
    for(;;){
        int n=SSL_read(ssl,buf,sizeof(buf));
        if(n<=0)break;
        if(fwrite(buf,1,n,p)!=static_cast<size_t>(n))break;
        if(fflush(p)!=0)break;
    }
    pclose(p);
}

static TlsConn open_video(SSL_CTX*ctx,const std::string&target,int port,bool tunneled,const std::string&token,const std::string&fingerprint){
    auto v=tunneled?connect_tls_retry(ctx,target,static_cast<uint16_t>(port),3000,100):connect_tls(ctx,target,static_cast<uint16_t>(port));
    if(!v.ssl)return{};
    if(!fingerprint.empty()&&!secure_equal(peer_fingerprint(v.ssl),fingerprint)){close_tls(v);return{};}
    if(!tls_write_line(v.ssl,"VIDEO "+token)){close_tls(v);return{};}
    std::string ready;
    if(!tls_read_line(v.ssl,ready)||ready!="READY"){close_tls(v);return{};}
    return v;
}

static void video_supervisor(SSL_CTX*ctx,const std::string target,int port,bool tunneled,const std::string token,const std::string fingerprint,TlsConn first,std::atomic<bool>*run,std::atomic<int>*active_fd){
    TlsConn v=first;
    bool recovering=false;
    while(run->load()){
        if(!v.ssl){
            v=open_video(ctx,target,port,tunneled,token,fingerprint);
            if(!v.ssl){std::this_thread::sleep_for(std::chrono::milliseconds(150));continue;}
            if(recovering){std::cout<<"Video restored.\n"<<std::flush;recovering=false;}
        }
        if(!run->load()){close_tls(v);break;}
        active_fd->store(v.fd);
        video_player(v.ssl);
        active_fd->store(-1);
        close_tls(v);
        if(run->load()){
            if(!recovering){std::cout<<"Video stalled; reconnecting...\n"<<std::flush;recovering=true;}
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    active_fd->store(-1);
    close_tls(v);
}

static void release_held(SSL*ssl,HeldInputState&held){for(const auto&command:held.release_commands())send_control(ssl,command);}
static bool release_chord(const HeldInputState&held,int code){
    bool ctrl=held.key_down(KEY_LEFTCTRL)||held.key_down(KEY_RIGHTCTRL);
    bool alt=held.key_down(KEY_LEFTALT)||held.key_down(KEY_RIGHTALT);
    bool shift=held.key_down(KEY_LEFTSHIFT)||held.key_down(KEY_RIGHTSHIFT);
    return code==KEY_Q&&ctrl&&alt&&shift;
}

static bool setup_xinput2(Display*d,Window root,int&opcode){
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

static bool raw_motion_values(const XIRawEvent*raw,double&dx,double&dy){
    dx=0.0;dy=0.0;if(!raw||!raw->valuators.mask||!raw->raw_values)return false;
    int value_index=0;
    for(int axis=0;axis<raw->valuators.mask_len*8;axis++){
        if(!XIMaskIsSet(raw->valuators.mask,axis))continue;
        double value=raw->raw_values[value_index++];
        if(axis==0)dx=value;else if(axis==1)dy=value;
    }
    return dx!=0.0||dy!=0.0;
}

static void run_xinput2_control(Display*d,Window root,int opcode,SSL*control){
    HeldInputState held;
    XGrabKeyboard(d,root,True,GrabModeAsync,GrabModeAsync,CurrentTime);
    XGrabPointer(d,root,True,0,GrabModeAsync,GrabModeAsync,None,None,CurrentTime);
    XFlush(d);
    bool run=true;
    while(run){
        XEvent event;XNextEvent(d,&event);
        if(event.xcookie.type!=GenericEvent||event.xcookie.extension!=opcode)continue;
        if(!XGetEventData(d,&event.xcookie))continue;
        auto*raw=static_cast<XIRawEvent*>(event.xcookie.data);bool send_ok=true;
        switch(event.xcookie.evtype){
            case XI_RawKeyPress:
            case XI_RawKeyRelease:{
                int code=linux_keycode_from_x11(static_cast<unsigned int>(raw->detail));
                bool down=event.xcookie.evtype==XI_RawKeyPress;
                if(code>0){
                    if(down&&release_chord(held,code)){run=false;break;}
                    if(down)held.press_key(code);else held.release_key(code);
                    send_ok=send_control(control,"KEY "+std::to_string(code)+" "+(down?"1":"0"));
                }
                break;
            }
            case XI_RawButtonPress:
            case XI_RawButtonRelease:{
                int button=raw->detail;bool down=event.xcookie.evtype==XI_RawButtonPress;
                if((button==4||button==5)&&down)send_ok=send_control(control,"WHEEL "+std::to_string(button==4?1:-1));
                else if(button>=1&&button<=3){if(down)held.press_button(button);else held.release_button(button);send_ok=send_control(control,"BUTTON "+std::to_string(button)+" "+(down?"1":"0"));}
                break;
            }
            case XI_RawMotion:{
                double dx=0.0,dy=0.0;
                if(raw_motion_values(raw,dx,dy)){auto command=raw_motion_command(dx,dy);if(!command.empty())send_ok=send_control(control,command);}
                break;
            }
            default:break;
        }
        XFreeEventData(d,&event.xcookie);
        if(!send_ok)run=false;
    }
    release_held(control,held);
    XUngrabKeyboard(d,CurrentTime);XUngrabPointer(d,CurrentTime);XFlush(d);
}

static void run_x11_fallback_control(Display*d,Window root,SSL*control){
    XWindowAttributes wa{};XGetWindowAttributes(d,root,&wa);int cx=wa.width/2,cy=wa.height/2;
    HeldInputState held;
    XGrabKeyboard(d,root,True,GrabModeAsync,GrabModeAsync,CurrentTime);
    XGrabPointer(d,root,True,PointerMotionMask|ButtonPressMask|ButtonReleaseMask,GrabModeAsync,GrabModeAsync,None,None,CurrentTime);
    XWarpPointer(d,None,root,0,0,0,0,cx,cy);XFlush(d);
    bool run=true;
    while(run){
        XEvent e;XNextEvent(d,&e);
        if(e.type==KeyPress||e.type==KeyRelease){
            auto*k=&e.xkey;KeySym sym=XLookupKeysym(k,0);
            if(e.type==KeyPress&&sym==XK_q&&(k->state&ControlMask)&&(k->state&Mod1Mask)&&(k->state&ShiftMask)){run=false;break;}
            int code=linux_keycode_from_x11(k->keycode);bool down=e.type==KeyPress;
            if(code>0){if(down)held.press_key(code);else held.release_key(code);if(!send_control(control,"KEY "+std::to_string(code)+" "+(down?"1":"0"))){run=false;break;}}
        }else if(e.type==MotionNotify){
            int dx=e.xmotion.x_root-cx,dy=e.xmotion.y_root-cy;
            if(dx||dy){if(!send_control(control,"MOUSE "+std::to_string(dx)+" "+std::to_string(dy))){run=false;break;}XWarpPointer(d,None,root,0,0,0,0,cx,cy);XFlush(d);}
        }else if(e.type==ButtonPress||e.type==ButtonRelease){
            int b=e.xbutton.button;bool down=e.type==ButtonPress,ok=true;
            if((b==4||b==5)&&down)ok=send_control(control,"WHEEL "+std::to_string(b==4?1:-1));
            else if(b<=3){if(down)held.press_button(b);else held.release_button(b);ok=send_control(control,"BUTTON "+std::to_string(b)+" "+(down?"1":"0"));}
            if(!ok){run=false;break;}
        }
    }
    release_held(control,held);
    XUngrabKeyboard(d,CurrentTime);XUngrabPointer(d,CurrentTime);XFlush(d);
}

int hosts_add(const std::string&name,const std::string&address,const std::string&mac){auto p=Paths::load();ensure_layout(p);Ini h;h.load(p.hosts);h.set(name,"address",address);h.set(name,"port","47990");h.set(name,"video_port","47991");if(!mac.empty())h.set(name,"mac",mac);if(!h.save(p.hosts))return 1;std::cout<<"saved host "<<name<<" -> "<<address<<"\n";return 0;}
int hosts_list(){auto p=Paths::load();Ini h;if(!h.load(p.hosts)){std::cout<<"No saved hosts.\n";return 0;}for(auto&[s,v]:h.sections())if(!s.empty()){auto it=v.find("address");std::cout<<s<<(it==v.end()?"":"  "+it->second)<<"\n";}return 0;}

int client_connect(const std::string&target_in,const std::string&password_arg){
    auto p=Paths::load();ensure_layout(p);ensure_identity(p.identity_key,p.identity_pub);
    Ini hosts;hosts.load(p.hosts);
    std::string target=target_in;std::string saved_address;
    int cp=47990,vp=47991;
    bool saved=hosts.sections().count(target)>0;bool tunneled=false;
    if(saved){target=hosts.get(target_in,"address");saved_address=target;cp=hosts.get_int(target_in,"port",47990);vp=hosts.get_int(target_in,"video_port",47991);}
    std::string control_token,video_token;
    if(tunnel_connection_code(target,&control_token,&video_token)){
        if(!tunnel_access(control_token,video_token)){std::cerr<<"OPAL tunnel access failed\n";return 2;}
        tunneled=true;target="127.0.0.1";cp=47990;vp=47991;
    }else if(target.rfind("zrok:",0)==0){
        auto tokens=target.substr(5);auto comma=tokens.find(',');
        if(comma==std::string::npos||!tunnel_access(tokens.substr(0,comma),tokens.substr(comma+1))){std::cerr<<"zrok access failed\n";return 2;}
        tunneled=true;target="127.0.0.1";cp=47990;vp=47991;
    }

    SSL_CTX*ctx=client_tls_context();
    auto c=tunneled?connect_tls_retry(ctx,target,static_cast<uint16_t>(cp)):connect_tls(ctx,target,static_cast<uint16_t>(cp));
    if(!c.ssl){std::cerr<<"cannot connect to OPAL host\n";SSL_CTX_free(ctx);return 1;}
    auto fingerprint=peer_fingerprint(c.ssl);
    if(saved){auto expected=hosts.get(target_in,"fingerprint");if(!expected.empty()&&!secure_equal(expected,fingerprint)){std::cerr<<"host certificate changed; refusing connection\n";close_tls(c);SSL_CTX_free(ctx);return 3;}}

    std::string challenge;
    if(!tls_read_line(c.ssl,challenge)||challenge.rfind("CHALLENGE ",0)!=0){close_tls(c);SSL_CTX_free(ctx);return 1;}
    auto nonce=challenge.substr(10),pub=public_key_hex(p.identity_pub);
    std::string auth;
    if(saved&&hosts.get(target_in,"paired")=="true")auth="AUTH "+pub+" "+sign_hex(p.identity_key,nonce);
    else{
        std::string password=password_arg;
        if(password.empty()){std::cout<<"Pairing password: ";std::cin>>password;}
        auth="PAIR "+pub+" "+hmac_sha256_hex(password,nonce+pub)+" "+target_in;
    }
    tls_write_line(c.ssl,auth);
    std::string reply;
    if(!tls_read_line(c.ssl,reply)||reply.rfind("OK ",0)!=0){std::cerr<<"authentication denied\n";close_tls(c);SSL_CTX_free(ctx);return 4;}
    auto token=reply.substr(3);
    if(!saved){hosts.set(target_in,"address",saved_address.empty()?target_in:saved_address);hosts.set(target_in,"port",std::to_string(cp));hosts.set(target_in,"video_port",std::to_string(vp));}
    hosts.set(target_in,"fingerprint",fingerprint);hosts.set(target_in,"paired","true");hosts.save(p.hosts);

    auto initial=open_video(ctx,target,vp,tunneled,token,fingerprint);
    if(!initial.ssl){std::cerr<<"video TLS connection failed\n";close_tls(c);SSL_CTX_free(ctx);return 1;}
    std::cout<<"Connected. Ctrl+Alt+Shift+Q releases remote control.\n";

    std::atomic<bool> video_run{true};
    std::atomic<int> active_video_fd{-1};
    TlsConn first{initial.fd,initial.ssl};initial={};
    std::thread player(video_supervisor,ctx,target,vp,tunneled,token,fingerprint,first,&video_run,&active_video_fd);

    Display*d=XOpenDisplay(nullptr);
    if(!d){
        std::cerr<<"DISPLAY unavailable; video only\n";
        while(video_run.load()){
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if(!send_control(c.ssl,"PING"))break;
            std::string pong;
            if(!tls_read_line(c.ssl,pong)||pong!="PONG")break;
        }
        video_run.store(false);
        int fd=active_video_fd.exchange(-1);if(fd>=0)shutdown(fd,SHUT_RDWR);
        player.join();close_tls(c);SSL_CTX_free(ctx);return 0;
    }

    Window root=DefaultRootWindow(d);int xi_opcode=0;
    if(setup_xinput2(d,root,xi_opcode))run_xinput2_control(d,root,xi_opcode,c.ssl);
    else run_x11_fallback_control(d,root,c.ssl);
    XCloseDisplay(d);

    video_run.store(false);int fd=active_video_fd.exchange(-1);if(fd>=0)shutdown(fd,SHUT_RDWR);
    player.join();close_tls(c);SSL_CTX_free(ctx);return 0;
}
}
