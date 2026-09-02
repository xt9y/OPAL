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

bool HeldInputState::press_key(int code){if(code<=0)return false;return keys_.insert(code).second;}
bool HeldInputState::release_key(int code){if(code<=0)return false;return keys_.erase(code)!=0;}
bool HeldInputState::press_button(int button){if(button<1||button>3)return false;return buttons_.insert(button).second;}
bool HeldInputState::release_button(int button){if(button<1||button>3)return false;return buttons_.erase(button)!=0;}
std::vector<std::string> HeldInputState::release_commands(){
    std::vector<std::string> out;
    for(int code:keys_)out.push_back("KEY "+std::to_string(code)+" 0");
    for(int button:buttons_)out.push_back("BUTTON "+std::to_string(button)+" 0");
    keys_.clear();buttons_.clear();
    return out;
}
}
