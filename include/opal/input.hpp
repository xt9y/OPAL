#pragma once
#include <set>
#include <string>
#include <vector>

namespace opal {
int linux_keycode_from_x11(unsigned int keycode);
std::string raw_motion_command(double dx,double dy);
std::string normalized_motion_command(double dx,double dy,int resolution_x,int resolution_y);

class HeldInputState {
    std::set<int> keys_;
    std::set<int> buttons_;
public:
    bool press_key(int code);
    bool release_key(int code);
    bool press_button(int button);
    bool release_button(int button);
    bool key_down(int code) const;
    bool button_down(int button) const;
    std::vector<std::string> release_commands();
};
}
