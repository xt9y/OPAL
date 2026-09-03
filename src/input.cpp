#include <opal/input.hpp>
#include <algorithm>
#include <cstdint>
#ifdef __linux__
#include <linux/input-event-codes.h>
#endif

namespace opal {
int linux_keycode_from_x11(unsigned int keycode){if(keycode<=8)return 0;unsigned int code=keycode-8;
#ifdef __linux__
if(code>KEY_MAX)return 0;
#else
if(code>0x2ff)return 0;
#endif
return static_cast<int>(code);}
static long rounded_delta(double value){return value>=0.0?static_cast<long>(value+0.5):static_cast<long>(value-0.5);}static std::string motion_command(double dx,double dy){long x=rounded_delta(dx),y=rounded_delta(dy);if(x==0&&y==0)return{};return "MOUSE "+std::to_string(x)+" "+std::to_string(y);}std::string raw_motion_command(double dx,double dy){return motion_command(dx,dy);}
// Kept for compatibility with existing host configuration. The exact-pointer
// path below does not use relative DPI normalization or sensitivity at all.
double mouse_normalization_scale(int){return 1.0;}double clamp_mouse_sensitivity(double sensitivity){return std::clamp(sensitivity,0.1,4.0);}std::string normalized_motion_command(double dx,double dy,int,int,double sensitivity){const double user_scale=clamp_mouse_sensitivity(sensitivity);return motion_command(dx*user_scale,dy*user_scale);}
std::string absolute_pointer_command(int x,int y,int width,int height){if(width<=0||height<=0)return{};x=std::clamp(x,0,width-1);y=std::clamp(y,0,height-1);auto scale=[](int value,int extent){if(extent<=1)return 0;const std::int64_t denominator=static_cast<std::int64_t>(extent-1);return static_cast<int>((static_cast<std::int64_t>(value)*pointer_abs_max+denominator/2)/denominator);};return "POINTER "+std::to_string(scale(x,width))+" "+std::to_string(scale(y,height));}
std::string video_pointer_command(int x,int y,int client_width,int client_height,int remote_width,int remote_height){if(client_width<=0||client_height<=0)return{};if(remote_width<=0||remote_height<=0)return absolute_pointer_command(x,y,client_width,client_height);int view_x=0,view_y=0,view_width=client_width,view_height=client_height;const std::int64_t client_cross=static_cast<std::int64_t>(client_width)*remote_height,remote_cross=static_cast<std::int64_t>(client_height)*remote_width;if(client_cross>remote_cross){view_width=static_cast<int>((static_cast<std::int64_t>(client_height)*remote_width)/remote_height);view_width=std::clamp(view_width,1,client_width);view_x=(client_width-view_width)/2;}else if(client_cross<remote_cross){view_height=static_cast<int>((static_cast<std::int64_t>(client_width)*remote_height)/remote_width);view_height=std::clamp(view_height,1,client_height);view_y=(client_height-view_height)/2;}return absolute_pointer_command(x-view_x,y-view_y,view_width,view_height);}
bool HeldInputState::press_key(int code){if(code<=0)return false;return keys_.insert(code).second;}bool HeldInputState::release_key(int code){if(code<=0)return false;return keys_.erase(code)!=0;}bool HeldInputState::press_button(int button){if(button<1||button>3)return false;return buttons_.insert(button).second;}bool HeldInputState::release_button(int button){if(button<1||button>3)return false;return buttons_.erase(button)!=0;}bool HeldInputState::key_down(int code)const{return keys_.find(code)!=keys_.end();}bool HeldInputState::button_down(int button)const{return buttons_.find(button)!=buttons_.end();}std::vector<std::string> HeldInputState::release_commands(){std::vector<std::string> out;for(int code:keys_)out.push_back("KEY "+std::to_string(code)+" 0");for(int button:buttons_)out.push_back("BUTTON "+std::to_string(button)+" 0");keys_.clear();buttons_.clear();return out;}
}
