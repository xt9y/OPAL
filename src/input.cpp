#include <opal/input.hpp>
#ifdef __linux__
#include <linux/input-event-codes.h>
#endif

namespace opal {
int linux_keycode_from_x11(unsigned int keycode){
    if(keycode<=8)return 0;
    unsigned int code=keycode-8;
#ifdef __linux__
    if(code>KEY_MAX)return 0;
#else
    if(code>0x2ff)return 0;
#endif
    return static_cast<int>(code);
}

static long rounded_delta(double value){return value>=0.0?static_cast<long>(value+0.5):static_cast<long>(value-0.5);}
static std::string motion_command(double dx,double dy){
    long x=rounded_delta(dx),y=rounded_delta(dy);
    if(x==0&&y==0)return{};
    return "MOUSE "+std::to_string(x)+" "+std::to_string(y);
}
std::string raw_motion_command(double dx,double dy){return motion_command(dx,dy);}
std::string normalized_motion_command(double dx,double dy,int resolution_x,int resolution_y){
    constexpr double target_counts_per_meter=1000.0/0.0254;
    // XI2/XWayland may expose synthetic resolutions such as 0 or 1. Only
    // treat a value as physical sensor resolution when it is plausible.
    if(resolution_x>=1000)dx=dx*target_counts_per_meter/static_cast<double>(resolution_x);
    if(resolution_y>=1000)dy=dy*target_counts_per_meter/static_cast<double>(resolution_y);
    return motion_command(dx,dy);
}

bool HeldInputState::press_key(int code){if(code<=0)return false;return keys_.insert(code).second;}
bool HeldInputState::release_key(int code){if(code<=0)return false;return keys_.erase(code)!=0;}
bool HeldInputState::press_button(int button){if(button<1||button>3)return false;return buttons_.insert(button).second;}
bool HeldInputState::release_button(int button){if(button<1||button>3)return false;return buttons_.erase(button)!=0;}
bool HeldInputState::key_down(int code) const{return keys_.find(code)!=keys_.end();}
bool HeldInputState::button_down(int button) const{return buttons_.find(button)!=buttons_.end();}
std::vector<std::string> HeldInputState::release_commands(){
    std::vector<std::string> out;
    for(int code:keys_)out.push_back("KEY "+std::to_string(code)+" 0");
    for(int button:buttons_)out.push_back("BUTTON "+std::to_string(button)+" 0");
    keys_.clear();buttons_.clear();
    return out;
}
}
