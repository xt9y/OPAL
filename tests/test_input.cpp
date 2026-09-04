#include <opal/input.hpp>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iterator>
#include <linux/input-event-codes.h>
#include <string>
#include <vector>

static std::string read_file(const char*path){std::ifstream f(path);return std::string((std::istreambuf_iterator<char>(f)),{});}
static bool near(double a,double b,double eps=0.0001){return std::fabs(a-b)<=eps;}

int main(){
    assert(opal::linux_keycode_from_sdl_scancode(4)==KEY_A);
    assert(opal::linux_keycode_from_sdl_scancode(20)==KEY_Q);
    assert(opal::linux_keycode_from_sdl_scancode(26)==KEY_W);
    assert(opal::linux_keycode_from_sdl_scancode(224)==KEY_LEFTCTRL);
    assert(opal::linux_keycode_from_sdl_scancode(9999)==0);
    assert(opal::raw_motion_command(12.0,-7.0)=="MOUSE 12 -7");

    assert(near(opal::mouse_normalization_scale(0),1.0));
    assert(near(opal::mouse_normalization_scale(1000),1.0));
    assert(near(opal::clamp_mouse_sensitivity(0.01),0.1));
    assert(near(opal::clamp_mouse_sensitivity(1.0),1.0));
    assert(near(opal::clamp_mouse_sensitivity(9.0),4.0));
    assert(opal::normalized_motion_command(32.0,-16.0,125984,125984)=="MOUSE 32 -16");

    assert(opal::absolute_pointer_command(0,0,1920,1080)=="POINTER 0 0");
    assert(opal::absolute_pointer_command(1919,1079,1920,1080)=="POINTER 65535 65535");
    assert(opal::absolute_pointer_command(500,500,1001,1001)=="POINTER 32768 32768");
    assert(opal::absolute_pointer_command(-50,5000,1920,1080)=="POINTER 0 65535");
    assert(opal::absolute_pointer_command(0,0,0,1080).empty());

    assert(opal::video_pointer_command(0,60,1920,1200,1920,1080)=="POINTER 0 0");
    assert(opal::video_pointer_command(1919,1139,1920,1200,1920,1080)=="POINTER 65535 65535");
    assert(opal::video_pointer_command(960,600,1920,1200,1920,1080)=="POINTER 32785 32798");

    opal::HeldInputState held;
    assert(held.press_key(30));assert(!held.press_key(30));assert(held.press_key(42));assert(held.release_key(30));assert(!held.release_key(30));assert(held.press_button(1));assert(held.release_button(1));assert(held.press_button(3));
    auto releases=held.release_commands();assert((releases==std::vector<std::string>{"KEY 42 0","BUTTON 3 0"}));assert(held.release_commands().empty());

    opal::HeldInputState chord;assert(chord.press_key(KEY_LEFTCTRL));assert(chord.press_key(KEY_LEFTALT));assert(chord.press_key(KEY_LEFTSHIFT));assert(opal::client_control_chord(chord,KEY_W)==opal::ClientControlChord::ReleaseCapture);assert(opal::client_control_chord(chord,KEY_Q)==opal::ClientControlChord::Quit);assert(opal::client_control_chord(chord,KEY_A)==opal::ClientControlChord::None);chord.release_key(KEY_LEFTSHIFT);assert(opal::client_control_chord(chord,KEY_W)==opal::ClientControlChord::None);

    auto helper=read_file("src/input_helper.cpp");assert(helper.find("KEY_MAX")!=std::string::npos);assert(helper.find("UI_SET_EVBIT,EV_ABS")!=std::string::npos);assert(helper.find("UI_SET_ABSBIT,ABS_X")!=std::string::npos);assert(helper.find("UI_SET_ABSBIT,ABS_Y")!=std::string::npos);assert(helper.find("OPAL Remote Absolute Pointer")!=std::string::npos);

    auto client=read_file("src/client.cpp");
    assert(client.find("<SDL3/SDL.h>")!=std::string::npos);
    assert(client.find("SDL_EVENT_KEY_DOWN")!=std::string::npos);
    assert(client.find("SDL_EVENT_MOUSE_MOTION")!=std::string::npos);
    assert(client.find("SDL_SetWindowRelativeMouseMode")==std::string::npos);
    assert(client.find("presenter.set_relative_mouse_mode")!=std::string::npos);
    assert(client.find("session.take_latest_video")!=std::string::npos);
    assert(client.find("XOpenDisplay")==std::string::npos);
    assert(client.find("XInput2")==std::string::npos);
    assert(client.find("presentation_window")==std::string::npos);
    assert(client.find("click the OPAL screen to capture again")!=std::string::npos);

    auto session_header=read_file("include/opal/session.hpp");auto session_source=read_file("src/session.cpp");
    assert(session_header.find("take_latest_video")!=std::string::npos);
    assert(session_header.find("presentation_window")==std::string::npos);
    assert(session_source.find("unhealthy reason=")!=std::string::npos);

    auto host=read_file("src/host.cpp");assert(host.find("SDL_GetDesktopDisplayMode")!=std::string::npos);assert(host.find("XOpenDisplay")==std::string::npos);
    auto makefile=read_file("Makefile");assert(makefile.find("sdl3")!=std::string::npos);assert(makefile.find("filter-out -lX11 -lXi")!=std::string::npos);
    return 0;
}
