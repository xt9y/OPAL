#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace opal {
inline constexpr int pointer_abs_max=65535;

enum class ClientControlChord {
    None,
    Quit,
    ReleaseCapture
};

int linux_keycode_from_sdl_scancode(int scancode);
std::string raw_motion_command(double dx,double dy);
double mouse_normalization_scale(int resolution);
double clamp_mouse_sensitivity(double sensitivity);
std::string normalized_motion_command(double dx,double dy,int resolution_x,int resolution_y,double sensitivity=1.0);
std::string absolute_pointer_command(int x,int y,int width,int height);
std::string video_pointer_command(int x,int y,int client_width,int client_height,int remote_width,int remote_height);

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

ClientControlChord client_control_chord(const HeldInputState&held,int code);
}
