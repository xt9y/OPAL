#ifdef __linux__
#include <linux/uinput.h>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cstring>
#include <iostream>
#include <set>
#include <sstream>

static constexpr int pointer_max=65535;

static bool emit_event(int fd,int type,int code,int value){input_event e{};e.type=type;e.code=code;e.value=value;for(;;){ssize_t n=write(fd,&e,sizeof(e));if(n==static_cast<ssize_t>(sizeof(e)))return true;if(n<0&&errno==EINTR)continue;return false;}}
static int button_code(int button){return button==1?BTN_LEFT:button==2?BTN_MIDDLE:button==3?BTN_RIGHT:0;}
static bool sync_events(int fd){return emit_event(fd,EV_SYN,SYN_REPORT,0);}

static int open_uinput(){return open("/dev/uinput",O_WRONLY|O_CLOEXEC);}

static bool create_device(int fd,const char*name,unsigned short product){
    uinput_setup us{};
    std::strncpy(us.name,name,UINPUT_MAX_NAME_SIZE-1);
    us.id.bustype=BUS_USB;us.id.vendor=0x4f50;us.id.product=product;
    return ioctl(fd,UI_DEV_SETUP,&us)>=0&&ioctl(fd,UI_DEV_CREATE)>=0;
}

static int create_keyboard(){
    int fd=open_uinput();if(fd<0)return -1;
    if(ioctl(fd,UI_SET_EVBIT,EV_KEY)<0){close(fd);return -1;}
    for(int i=1;i<=KEY_MAX;i++)ioctl(fd,UI_SET_KEYBIT,i);
    if(!create_device(fd,"OPAL Remote Keyboard",0x414b)){close(fd);return -1;}
    return fd;
}

static bool setup_abs_axis(int fd,int code){
    uinput_abs_setup axis{};
    axis.code=static_cast<__u16>(code);
    axis.absinfo.minimum=0;
    axis.absinfo.maximum=pointer_max;
    axis.absinfo.fuzz=0;
    axis.absinfo.flat=0;
    axis.absinfo.resolution=1;
    return ioctl(fd,UI_ABS_SETUP,&axis)>=0;
}

static int create_pointer(){
    int fd=open_uinput();if(fd<0)return -1;
    bool ok=true;
    ok=ok&&ioctl(fd,UI_SET_EVBIT,EV_KEY)>=0;
    ok=ok&&ioctl(fd,UI_SET_KEYBIT,BTN_LEFT)>=0;
    ok=ok&&ioctl(fd,UI_SET_KEYBIT,BTN_MIDDLE)>=0;
    ok=ok&&ioctl(fd,UI_SET_KEYBIT,BTN_RIGHT)>=0;
    ok=ok&&ioctl(fd,UI_SET_EVBIT,EV_ABS)>=0;
    ok=ok&&ioctl(fd,UI_SET_ABSBIT,ABS_X)>=0;
    ok=ok&&ioctl(fd,UI_SET_ABSBIT,ABS_Y)>=0;
    ok=ok&&ioctl(fd,UI_SET_EVBIT,EV_REL)>=0;
    ok=ok&&ioctl(fd,UI_SET_RELBIT,REL_WHEEL)>=0;
    ok=ok&&ioctl(fd,UI_SET_PROPBIT,INPUT_PROP_DIRECT)>=0;
    ok=ok&&ioctl(fd,UI_SET_PROPBIT,INPUT_PROP_POINTER)>=0;
    ok=ok&&setup_abs_axis(fd,ABS_X)&&setup_abs_axis(fd,ABS_Y);
    if(!ok||!create_device(fd,"OPAL Remote Absolute Pointer",0x4150)){close(fd);return -1;}
    return fd;
}

static void destroy_device(int fd){if(fd<0)return;ioctl(fd,UI_DEV_DESTROY);close(fd);}

int main(){
    int keyboard_fd=create_keyboard();
    if(keyboard_fd<0){perror("OPAL keyboard uinput");return 1;}
    int pointer_fd=create_pointer();
    if(pointer_fd<0){perror("OPAL pointer uinput");destroy_device(keyboard_fd);return 1;}

    std::set<int> held_keys,held_buttons;
    std::string line;
    while(std::getline(std::cin,line)){
        std::istringstream ss(line);std::string t;ss>>t;bool emitted=false;
        if(t=="KEY"){
            int k,d;if(ss>>k>>d&&k>0&&k<=KEY_MAX&&(d==0||d==1)){
                if(emit_event(keyboard_fd,EV_KEY,k,d)&&sync_events(keyboard_fd)){if(d)held_keys.insert(k);else held_keys.erase(k);}
            }
        }else if(t=="POINTER"){
            int x,y;if(ss>>x>>y&&x>=0&&x<=pointer_max&&y>=0&&y<=pointer_max){
                bool xok=emit_event(pointer_fd,EV_ABS,ABS_X,x);
                bool yok=emit_event(pointer_fd,EV_ABS,ABS_Y,y);
                emitted=xok&&yok;
            }
        }else if(t=="BUTTON"){
            int b,d;if(ss>>b>>d&&(d==0||d==1)){
                int code=button_code(b);
                if(code&&emit_event(pointer_fd,EV_KEY,code,d)){if(d)held_buttons.insert(code);else held_buttons.erase(code);emitted=true;}
            }
        }else if(t=="WHEEL"){
            int v;if(ss>>v)emitted=emit_event(pointer_fd,EV_REL,REL_WHEEL,v);
        }
        if(emitted&&!sync_events(pointer_fd))break;
    }

    bool keyboard_released=false;
    for(int code:held_keys){keyboard_released|=emit_event(keyboard_fd,EV_KEY,code,0);}
    if(keyboard_released)sync_events(keyboard_fd);
    bool pointer_released=false;
    for(int code:held_buttons){pointer_released|=emit_event(pointer_fd,EV_KEY,code,0);}
    if(pointer_released)sync_events(pointer_fd);
    destroy_device(pointer_fd);destroy_device(keyboard_fd);return 0;
}
#else
#include <iostream>
int main(){std::cerr<<"opal-input is Linux-only\n";return 1;}
#endif
