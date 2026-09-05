#include <opal/client.hpp>
#include <opal/input.hpp>
#include <cassert>
#include <cmath>
#include <linux/input-event-codes.h>
#include <string>
#include <vector>

static bool near(double a,double b,double eps=0.0001){return std::fabs(a-b)<=eps;}

int main(){
    static_assert(opal::kClientIdleWaitNs==200000);
    assert(opal::linux_keycode_from_sdl_scancode(4)==KEY_A);
    assert(opal::linux_keycode_from_sdl_scancode(20)==KEY_Q);
    assert(opal::linux_keycode_from_sdl_scancode(26)==KEY_W);
    assert(opal::linux_keycode_from_sdl_scancode(224)==KEY_LEFTCTRL);
    assert(opal::linux_keycode_from_sdl_scancode(9999)==0);

    assert(opal::raw_motion_command(12.0,-7.0)=="MOUSE 12 -7");
    assert(opal::raw_motion_command(0.2,-0.2).empty());
    assert(opal::raw_motion_command(0.6,-0.6)=="MOUSE 1 -1");
    assert(near(opal::mouse_normalization_scale(0),1.0));
    assert(near(opal::mouse_normalization_scale(1000),1.0));
    assert(near(opal::clamp_mouse_sensitivity(0.01),0.1));
    assert(near(opal::clamp_mouse_sensitivity(1.0),1.0));
    assert(near(opal::clamp_mouse_sensitivity(9.0),4.0));
    assert(opal::normalized_motion_command(32.0,-16.0,125984,125984)=="MOUSE 32 -16");
    assert(opal::normalized_motion_command(32.0,-16.0,125984,125984,0.5)=="MOUSE 16 -8");

    assert(opal::absolute_pointer_command(0,0,1920,1080)=="POINTER 0 0");
    assert(opal::absolute_pointer_command(1919,1079,1920,1080)=="POINTER 65535 65535");
    assert(opal::absolute_pointer_command(500,500,1001,1001)=="POINTER 32768 32768");
    assert(opal::absolute_pointer_command(-50,5000,1920,1080)=="POINTER 0 65535");
    assert(opal::absolute_pointer_command(0,0,0,1080).empty());

    assert(opal::video_pointer_command(0,60,1920,1200,1920,1080)=="POINTER 0 0");
    assert(opal::video_pointer_command(1919,1139,1920,1200,1920,1080)=="POINTER 65535 65535");
    assert(opal::video_pointer_command(960,600,1920,1200,1920,1080)=="POINTER 32785 32798");
    assert(opal::video_pointer_command(960,540,1920,1080,0,0)=="POINTER 32785 32798");

    opal::HeldInputState held;
    assert(held.press_key(30));
    assert(!held.press_key(30));
    assert(held.press_key(42));
    assert(held.release_key(30));
    assert(!held.release_key(30));
    assert(held.press_button(1));
    assert(held.release_button(1));
    assert(held.press_button(3));
    auto releases=held.release_commands();
    assert((releases==std::vector<std::string>{"KEY 42 0","BUTTON 3 0"}));
    assert(held.release_commands().empty());

    opal::HeldInputState chord;
    assert(chord.press_key(KEY_LEFTCTRL));
    assert(chord.press_key(KEY_LEFTALT));
    assert(chord.press_key(KEY_LEFTSHIFT));
    assert(opal::client_control_chord(chord,KEY_W)==opal::ClientControlChord::ReleaseCapture);
    assert(opal::client_control_chord(chord,KEY_Q)==opal::ClientControlChord::Quit);
    assert(opal::client_control_chord(chord,KEY_A)==opal::ClientControlChord::None);
    chord.release_key(KEY_LEFTSHIFT);
    assert(opal::client_control_chord(chord,KEY_W)==opal::ClientControlChord::None);
    return 0;
}
