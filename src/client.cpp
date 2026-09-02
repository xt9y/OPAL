#include <opal/client.hpp>
#include <opal/config.hpp>
#include <opal/crypto.hpp>
#include <opal/net.hpp>
#include <opal/tunnel.hpp>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
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
    while(run->load()){
        if(!v.ssl){
            v=open_video(ctx,target,port,tunneled,token,fingerprint);
            if(!v.ssl){std::this_thread::sleep_for(std::chrono::milliseconds(150));continue;}
        }
        if(!run->load()){close_tls(v);break;}
        active_fd->store(v.fd);
        video_player(v.ssl);
        active_fd->store(-1);
        close_tls(v);
        if(run->load())std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    active_fd->store(-1);
    close_tls(v);
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

    Window root=DefaultRootWindow(d);XWindowAttributes wa{};XGetWindowAttributes(d,root,&wa);int cx=wa.width/2,cy=wa.height/2;
    XGrabKeyboard(d,root,True,GrabModeAsync,GrabModeAsync,CurrentTime);
    XGrabPointer(d,root,True,PointerMotionMask|ButtonPressMask|ButtonReleaseMask,GrabModeAsync,GrabModeAsync,None,None,CurrentTime);
    XWarpPointer(d,None,root,0,0,0,0,cx,cy);XFlush(d);
    bool run=true;
    while(run){
        XEvent e;XNextEvent(d,&e);
        if(e.type==KeyPress||e.type==KeyRelease){
            auto*k=&e.xkey;KeySym sym=XLookupKeysym(k,0);
            if(e.type==KeyPress&&sym==XK_q&&(k->state&ControlMask)&&(k->state&Mod1Mask)&&(k->state&ShiftMask)){run=false;break;}
            int code=std::max(0,static_cast<int>(k->keycode)-8);
            if(!send_control(c.ssl,"KEY "+std::to_string(code)+" "+(e.type==KeyPress?"1":"0"))){run=false;break;}
        }else if(e.type==MotionNotify){
            int dx=e.xmotion.x_root-cx,dy=e.xmotion.y_root-cy;
            if(dx||dy){if(!send_control(c.ssl,"MOUSE "+std::to_string(dx)+" "+std::to_string(dy))){run=false;break;}XWarpPointer(d,None,root,0,0,0,0,cx,cy);XFlush(d);}
        }else if(e.type==ButtonPress||e.type==ButtonRelease){
            int b=e.xbutton.button;bool ok=true;
            if((b==4||b==5)&&e.type==ButtonPress)ok=send_control(c.ssl,"WHEEL "+std::to_string(b==4?1:-1));
            else if(b<=3)ok=send_control(c.ssl,"BUTTON "+std::to_string(b)+" "+(e.type==ButtonPress?"1":"0"));
            if(!ok){run=false;break;}
        }
    }

    XUngrabKeyboard(d,CurrentTime);XUngrabPointer(d,CurrentTime);XCloseDisplay(d);
    video_run.store(false);int fd=active_video_fd.exchange(-1);if(fd>=0)shutdown(fd,SHUT_RDWR);
    player.join();close_tls(c);SSL_CTX_free(ctx);return 0;
}
}
