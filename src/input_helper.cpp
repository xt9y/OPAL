#ifdef __linux__
#include <opal/input_record.hpp>

#include <linux/uinput.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <set>
#include <span>
#include <string_view>
#include <unistd.h>
#include <sys/ioctl.h>
#include <vector>

namespace {
constexpr int pointer_max=65535;

input_event make_event(int type,int code,int value){input_event event{};event.type=static_cast<__u16>(type);event.code=static_cast<__u16>(code);event.value=value;return event;}

bool write_events(int fd,std::span<const input_event>events){
    if(fd<0||events.empty())return false;
    const auto*bytes=reinterpret_cast<const std::uint8_t*>(events.data());
    std::size_t remaining=events.size_bytes();
    while(remaining){
        const ssize_t n=write(fd,bytes,remaining);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)return false;
        bytes+=static_cast<std::size_t>(n);remaining-=static_cast<std::size_t>(n);
    }
    return true;
}

int button_code(int button){return button==1?BTN_LEFT:button==2?BTN_MIDDLE:button==3?BTN_RIGHT:0;}
int open_uinput(){return open("/dev/uinput",O_WRONLY|O_CLOEXEC);}

bool create_device(int fd,const char*name,unsigned short product){
    uinput_setup setup{};
    std::strncpy(setup.name,name,UINPUT_MAX_NAME_SIZE-1);
    setup.id.bustype=BUS_USB;setup.id.vendor=0x4f50;setup.id.product=product;
    return ioctl(fd,UI_DEV_SETUP,&setup)>=0&&ioctl(fd,UI_DEV_CREATE)>=0;
}

int create_keyboard(){
    const int fd=open_uinput();if(fd<0)return -1;
    if(ioctl(fd,UI_SET_EVBIT,EV_KEY)<0){close(fd);return -1;}
    for(int i=1;i<=KEY_MAX;i++)ioctl(fd,UI_SET_KEYBIT,i);
    if(!create_device(fd,"OPAL Remote Keyboard",0x414b)){close(fd);return -1;}
    return fd;
}

bool setup_abs_axis(int fd,int code){
    uinput_abs_setup axis{};axis.code=static_cast<__u16>(code);axis.absinfo.minimum=0;axis.absinfo.maximum=pointer_max;axis.absinfo.resolution=1;
    return ioctl(fd,UI_ABS_SETUP,&axis)>=0;
}

int create_pointer(){
    const int fd=open_uinput();if(fd<0)return -1;
    bool ok=true;
    ok=ok&&ioctl(fd,UI_SET_EVBIT,EV_KEY)>=0;
    ok=ok&&ioctl(fd,UI_SET_KEYBIT,BTN_LEFT)>=0;
    ok=ok&&ioctl(fd,UI_SET_KEYBIT,BTN_MIDDLE)>=0;
    ok=ok&&ioctl(fd,UI_SET_KEYBIT,BTN_RIGHT)>=0;
    ok=ok&&ioctl(fd,UI_SET_EVBIT,EV_ABS)>=0;
    ok=ok&&ioctl(fd,UI_SET_ABSBIT,ABS_X)>=0;
    ok=ok&&ioctl(fd,UI_SET_ABSBIT,ABS_Y)>=0;
    ok=ok&&ioctl(fd,UI_SET_EVBIT,EV_REL)>=0;
    ok=ok&&ioctl(fd,UI_SET_RELBIT,REL_X)>=0;
    ok=ok&&ioctl(fd,UI_SET_RELBIT,REL_Y)>=0;
    ok=ok&&ioctl(fd,UI_SET_RELBIT,REL_WHEEL)>=0;
    ok=ok&&ioctl(fd,UI_SET_PROPBIT,INPUT_PROP_POINTER)>=0;
    ok=ok&&setup_abs_axis(fd,ABS_X)&&setup_abs_axis(fd,ABS_Y);
    if(!ok||!create_device(fd,"OPAL Remote Pointer",0x4150)){close(fd);return -1;}
    return fd;
}

void destroy_device(int fd){if(fd<0)return;ioctl(fd,UI_DEV_DESTROY);close(fd);}

bool apply_record(int keyboard_fd,int pointer_fd,const opal::InputRecord&record,std::set<int>&held_keys,std::set<int>&held_buttons){
    const auto sync=make_event(EV_SYN,SYN_REPORT,0);
    switch(record.type){
        case opal::InputRecordType::Key:{
            if(record.a<=0||record.a>KEY_MAX||(record.b!=0&&record.b!=1))return true;
            const std::array events{make_event(EV_KEY,record.a,record.b),sync};
            if(!write_events(keyboard_fd,events))return false;
            if(record.b)held_keys.insert(record.a);else held_keys.erase(record.a);
            return true;
        }
        case opal::InputRecordType::Pointer:{
            if(record.a<0||record.a>pointer_max||record.b<0||record.b>pointer_max)return true;
            const std::array events{make_event(EV_ABS,ABS_X,record.a),make_event(EV_ABS,ABS_Y,record.b),sync};
            return write_events(pointer_fd,events);
        }
        case opal::InputRecordType::Button:{
            const int code=button_code(record.a);if(!code||(record.b!=0&&record.b!=1))return true;
            const std::array events{make_event(EV_KEY,code,record.b),sync};
            if(!write_events(pointer_fd,events))return false;
            if(record.b)held_buttons.insert(code);else held_buttons.erase(code);
            return true;
        }
        case opal::InputRecordType::Wheel:{
            const std::array events{make_event(EV_REL,REL_WHEEL,record.a),sync};
            return write_events(pointer_fd,events);
        }
        case opal::InputRecordType::Relative:{
            if(record.a==0&&record.b==0)return true;
            const std::array events{make_event(EV_REL,REL_X,record.a),make_event(EV_REL,REL_Y,record.b),sync};
            return write_events(pointer_fd,events);
        }
    }
    return true;
}

bool consume_pending(std::vector<std::uint8_t>&pending,int keyboard_fd,int pointer_fd,std::set<int>&held_keys,std::set<int>&held_buttons,bool eof){
    for(;;){
        if(pending.empty())return true;
        const bool binary= pending.size()>=4&&pending[0]=='O'&&pending[1]=='P'&&pending[2]=='I'&&pending[3]=='N';
        if(binary){
            if(pending.size()<opal::kInputRecordBytes)return !eof;
            opal::InputRecord record;
            if(!opal::decode_input_record(std::span<const std::uint8_t>(pending.data(),opal::kInputRecordBytes),record))return false;
            if(!apply_record(keyboard_fd,pointer_fd,record,held_keys,held_buttons))return false;
            pending.erase(pending.begin(),pending.begin()+static_cast<std::ptrdiff_t>(opal::kInputRecordBytes));
            continue;
        }
        const auto newline=std::find(pending.begin(),pending.end(),static_cast<std::uint8_t>('\n'));
        if(newline==pending.end()){
            if(!eof){if(pending.size()>512)return false;return true;}
            if(pending.empty())return true;
        }
        const std::size_t length=newline==pending.end()?pending.size():static_cast<std::size_t>(newline-pending.begin());
        const std::string_view line(reinterpret_cast<const char*>(pending.data()),length);
        opal::InputRecord record;
        if(!line.empty()&&opal::parse_input_command(line,record)&&!apply_record(keyboard_fd,pointer_fd,record,held_keys,held_buttons))return false;
        const std::size_t consumed=length+(newline==pending.end()?0:1);
        pending.erase(pending.begin(),pending.begin()+static_cast<std::ptrdiff_t>(consumed));
        if(newline==pending.end())return true;
    }
}

void release_held(int keyboard_fd,int pointer_fd,const std::set<int>&held_keys,const std::set<int>&held_buttons){
    for(int code:held_keys){const std::array events{make_event(EV_KEY,code,0),make_event(EV_SYN,SYN_REPORT,0)};(void)write_events(keyboard_fd,events);}
    for(int code:held_buttons){const std::array events{make_event(EV_KEY,code,0),make_event(EV_SYN,SYN_REPORT,0)};(void)write_events(pointer_fd,events);}
}
}

int main(){
    const int keyboard_fd=create_keyboard();
    if(keyboard_fd<0){perror("OPAL keyboard uinput");return 1;}
    const int pointer_fd=create_pointer();
    if(pointer_fd<0){perror("OPAL pointer uinput");destroy_device(keyboard_fd);return 1;}

    std::set<int>held_keys,held_buttons;
    std::vector<std::uint8_t>pending;pending.reserve(1024);
    std::array<std::uint8_t,1024>chunk{};
    bool ok=true;
    for(;;){
        ssize_t n=read(STDIN_FILENO,chunk.data(),chunk.size());
        if(n<0&&errno==EINTR)continue;
        if(n<0){ok=false;break;}
        if(n==0){ok=consume_pending(pending,keyboard_fd,pointer_fd,held_keys,held_buttons,true);break;}
        pending.insert(pending.end(),chunk.begin(),chunk.begin()+n);
        if(!consume_pending(pending,keyboard_fd,pointer_fd,held_keys,held_buttons,false)){ok=false;break;}
    }

    release_held(keyboard_fd,pointer_fd,held_keys,held_buttons);
    destroy_device(pointer_fd);destroy_device(keyboard_fd);
    return ok?0:1;
}
#else
#include <iostream>
int main(){std::cerr<<"opal-input is Linux-only\n";return 1;}
#endif
