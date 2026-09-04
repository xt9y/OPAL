#include <opal/input.hpp>
#include <algorithm>
#include <cstdint>
#ifdef __linux__
#include <linux/input-event-codes.h>
#endif

namespace opal {
int linux_keycode_from_sdl_scancode(int s){
#ifdef __linux__
    switch(s){
        case 4:return KEY_A;case 5:return KEY_B;case 6:return KEY_C;case 7:return KEY_D;case 8:return KEY_E;case 9:return KEY_F;case 10:return KEY_G;case 11:return KEY_H;case 12:return KEY_I;case 13:return KEY_J;case 14:return KEY_K;case 15:return KEY_L;case 16:return KEY_M;case 17:return KEY_N;case 18:return KEY_O;case 19:return KEY_P;case 20:return KEY_Q;case 21:return KEY_R;case 22:return KEY_S;case 23:return KEY_T;case 24:return KEY_U;case 25:return KEY_V;case 26:return KEY_W;case 27:return KEY_X;case 28:return KEY_Y;case 29:return KEY_Z;
        case 30:return KEY_1;case 31:return KEY_2;case 32:return KEY_3;case 33:return KEY_4;case 34:return KEY_5;case 35:return KEY_6;case 36:return KEY_7;case 37:return KEY_8;case 38:return KEY_9;case 39:return KEY_0;
        case 40:return KEY_ENTER;case 41:return KEY_ESC;case 42:return KEY_BACKSPACE;case 43:return KEY_TAB;case 44:return KEY_SPACE;case 45:return KEY_MINUS;case 46:return KEY_EQUAL;case 47:return KEY_LEFTBRACE;case 48:return KEY_RIGHTBRACE;case 49:return KEY_BACKSLASH;case 50:return KEY_BACKSLASH;case 51:return KEY_SEMICOLON;case 52:return KEY_APOSTROPHE;case 53:return KEY_GRAVE;case 54:return KEY_COMMA;case 55:return KEY_DOT;case 56:return KEY_SLASH;case 57:return KEY_CAPSLOCK;
        case 58:return KEY_F1;case 59:return KEY_F2;case 60:return KEY_F3;case 61:return KEY_F4;case 62:return KEY_F5;case 63:return KEY_F6;case 64:return KEY_F7;case 65:return KEY_F8;case 66:return KEY_F9;case 67:return KEY_F10;case 68:return KEY_F11;case 69:return KEY_F12;
        case 70:return KEY_SYSRQ;case 71:return KEY_SCROLLLOCK;case 72:return KEY_PAUSE;case 73:return KEY_INSERT;case 74:return KEY_HOME;case 75:return KEY_PAGEUP;case 76:return KEY_DELETE;case 77:return KEY_END;case 78:return KEY_PAGEDOWN;case 79:return KEY_RIGHT;case 80:return KEY_LEFT;case 81:return KEY_DOWN;case 82:return KEY_UP;
        case 83:return KEY_NUMLOCK;case 84:return KEY_KPSLASH;case 85:return KEY_KPASTERISK;case 86:return KEY_KPMINUS;case 87:return KEY_KPPLUS;case 88:return KEY_KPENTER;case 89:return KEY_KP1;case 90:return KEY_KP2;case 91:return KEY_KP3;case 92:return KEY_KP4;case 93:return KEY_KP5;case 94:return KEY_KP6;case 95:return KEY_KP7;case 96:return KEY_KP8;case 97:return KEY_KP9;case 98:return KEY_KP0;case 99:return KEY_KPDOT;case 101:return KEY_COMPOSE;
        case 224:return KEY_LEFTCTRL;case 225:return KEY_LEFTSHIFT;case 226:return KEY_LEFTALT;case 227:return KEY_LEFTMETA;case 228:return KEY_RIGHTCTRL;case 229:return KEY_RIGHTSHIFT;case 230:return KEY_RIGHTALT;case 231:return KEY_RIGHTMETA;
        default:return 0;
    }
#else
    (void)s;return 0;
#endif
}
static long rounded_delta(double value){return value>=0.0?static_cast<long>(value+0.5):static_cast<long>(value-0.5);}static std::string motion_command(double dx,double dy){long x=rounded_delta(dx),y=rounded_delta(dy);if(x==0&&y==0)return{};return "MOUSE "+std::to_string(x)+" "+std::to_string(y);}std::string raw_motion_command(double dx,double dy){return motion_command(dx,dy);}
double mouse_normalization_scale(int){return 1.0;}double clamp_mouse_sensitivity(double sensitivity){return std::clamp(sensitivity,0.1,4.0);}std::string normalized_motion_command(double dx,double dy,int,int,double sensitivity){const double user_scale=clamp_mouse_sensitivity(sensitivity);return motion_command(dx*user_scale,dy*user_scale);}
std::string absolute_pointer_command(int x,int y,int width,int height){if(width<=0||height<=0)return{};x=std::clamp(x,0,width-1);y=std::clamp(y,0,height-1);auto scale=[](int value,int extent){if(extent<=1)return 0;const std::int64_t denominator=static_cast<std::int64_t>(extent-1);return static_cast<int>((static_cast<std::int64_t>(value)*pointer_abs_max+denominator/2)/denominator);};return "POINTER "+std::to_string(scale(x,width))+" "+std::to_string(scale(y,height));}
std::string video_pointer_command(int x,int y,int client_width,int client_height,int remote_width,int remote_height){if(client_width<=0||client_height<=0)return{};if(remote_width<=0||remote_height<=0)return absolute_pointer_command(x,y,client_width,client_height);int view_x=0,view_y=0,view_width=client_width,view_height=client_height;const std::int64_t client_cross=static_cast<std::int64_t>(client_width)*remote_height,remote_cross=static_cast<std::int64_t>(client_height)*remote_width;if(client_cross>remote_cross){view_width=static_cast<int>((static_cast<std::int64_t>(client_height)*remote_width)/remote_height);view_width=std::clamp(view_width,1,client_width);view_x=(client_width-view_width)/2;}else if(client_cross<remote_cross){view_height=static_cast<int>((static_cast<std::int64_t>(client_width)*remote_height)/remote_width);view_height=std::clamp(view_height,1,client_height);view_y=(client_height-view_height)/2;}return absolute_pointer_command(x-view_x,y-view_y,view_width,view_height);}
bool HeldInputState::press_key(int code){if(code<=0)return false;return keys_.insert(code).second;}bool HeldInputState::release_key(int code){if(code<=0)return false;return keys_.erase(code)!=0;}bool HeldInputState::press_button(int button){if(button<1||button>3)return false;return buttons_.insert(button).second;}bool HeldInputState::release_button(int button){if(button<1||button>3)return false;return buttons_.erase(button)!=0;}bool HeldInputState::key_down(int code)const{return keys_.find(code)!=keys_.end();}bool HeldInputState::button_down(int button)const{return buttons_.find(button)!=buttons_.end();}std::vector<std::string> HeldInputState::release_commands(){std::vector<std::string> out;for(int code:keys_)out.push_back("KEY "+std::to_string(code)+" 0");for(int button:buttons_)out.push_back("BUTTON "+std::to_string(button)+" 0");keys_.clear();buttons_.clear();return out;}
ClientControlChord client_control_chord(const HeldInputState&held,int code){
#ifdef __linux__
const bool ctrl=held.key_down(KEY_LEFTCTRL)||held.key_down(KEY_RIGHTCTRL);const bool alt=held.key_down(KEY_LEFTALT)||held.key_down(KEY_RIGHTALT);const bool shift=held.key_down(KEY_LEFTSHIFT)||held.key_down(KEY_RIGHTSHIFT);if(!(ctrl&&alt&&shift))return ClientControlChord::None;if(code==KEY_Q)return ClientControlChord::Quit;if(code==KEY_W)return ClientControlChord::ReleaseCapture;
#else
(void)held;(void)code;
#endif
return ClientControlChord::None;}
}
