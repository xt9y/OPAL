#ifdef __linux__
#include <linux/uinput.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cstring>
#include <iostream>
#include <set>
#include <sstream>

static bool emit_event(int fd,int type,int code,int value){input_event e{};e.type=type;e.code=code;e.value=value;return write(fd,&e,sizeof(e))==static_cast<ssize_t>(sizeof(e));}
static int button_code(int button){return button==1?BTN_LEFT:button==2?BTN_MIDDLE:button==3?BTN_RIGHT:0;}
static void sync_events(int fd){emit_event(fd,EV_SYN,SYN_REPORT,0);}

int main(){
    int fd=open("/dev/uinput",O_WRONLY|O_NONBLOCK);
    if(fd<0){perror("/dev/uinput");return 1;}
    ioctl(fd,UI_SET_EVBIT,EV_KEY);
    for(int i=1;i<=KEY_MAX;i++)ioctl(fd,UI_SET_KEYBIT,i);
    ioctl(fd,UI_SET_EVBIT,EV_REL);
    ioctl(fd,UI_SET_RELBIT,REL_X);ioctl(fd,UI_SET_RELBIT,REL_Y);ioctl(fd,UI_SET_RELBIT,REL_WHEEL);
    uinput_setup us{};std::strncpy(us.name,"OPAL Remote Input",UINPUT_MAX_NAME_SIZE-1);us.id.bustype=BUS_USB;us.id.vendor=0x4f50;us.id.product=0x414c;
    if(ioctl(fd,UI_DEV_SETUP,&us)<0||ioctl(fd,UI_DEV_CREATE)<0){perror("uinput setup");close(fd);return 1;}

    std::set<int> held_keys,held_buttons;
    std::string line;
    while(std::getline(std::cin,line)){
        std::istringstream ss(line);std::string t;ss>>t;bool emitted=false;
        if(t=="KEY"){
            int k,d;if(ss>>k>>d&&k>0&&k<=KEY_MAX&&(d==0||d==1)){
                if(emit_event(fd,EV_KEY,k,d)){if(d)held_keys.insert(k);else held_keys.erase(k);emitted=true;}
            }
        }else if(t=="MOUSE"){
            int x,y;if(ss>>x>>y){emit_event(fd,EV_REL,REL_X,x);emit_event(fd,EV_REL,REL_Y,y);emitted=true;}
        }else if(t=="BUTTON"){
            int b,d;if(ss>>b>>d&&(d==0||d==1)){
                int code=button_code(b);
                if(code&&emit_event(fd,EV_KEY,code,d)){if(d)held_buttons.insert(code);else held_buttons.erase(code);emitted=true;}
            }
        }else if(t=="WHEEL"){
            int v;if(ss>>v){emit_event(fd,EV_REL,REL_WHEEL,v);emitted=true;}
        }
        if(emitted)sync_events(fd);
    }

    bool released=false;
    for(int code:held_keys){emit_event(fd,EV_KEY,code,0);released=true;}
    for(int code:held_buttons){emit_event(fd,EV_KEY,code,0);released=true;}
    if(released)sync_events(fd);
    ioctl(fd,UI_DEV_DESTROY);close(fd);return 0;
}
#else
#include <iostream>
int main(){std::cerr<<"opal-input is Linux-only\n";return 1;}
#endif
