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
    assert(opal::linux_keycode_from_x11(9)==1);
    assert(opal::linux_keycode_from_x11(108)==100);
    assert(opal::linux_keycode_from_x11(0)==0);
    assert(opal::linux_keycode_from_x11(7)==0);
    assert(opal::linux_keycode_from_x11(8)==0);
    assert(opal::linux_keycode_from_x11(100000)==0);
    assert(opal::raw_motion_command(12.0,-7.0)=="MOUSE 12 -7");

    assert(near(opal::mouse_normalization_scale(0),1.0));
    assert(near(opal::mouse_normalization_scale(1000),1.0));
    assert(near(opal::mouse_normalization_scale(5000),1.0));
    assert(near(opal::mouse_normalization_scale(125984),1.0));
    assert(near(opal::mouse_normalization_scale(400000),1.0));
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
    assert(opal::video_pointer_command(500,0,1920,1200,1920,1080).rfind("POINTER ",0)==0);
    assert(opal::video_pointer_command(240,0,1920,1080,1440,1080)=="POINTER 0 0");
    assert(opal::video_pointer_command(1679,1079,1920,1080,1440,1080)=="POINTER 65535 65535");

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

    auto helper=read_file("src/input_helper.cpp");
    assert(helper.find("KEY_MAX")!=std::string::npos);
    assert(helper.find("k<256")==std::string::npos);
    assert(helper.find("i<256")==std::string::npos);
    assert(helper.find("UI_SET_EVBIT,EV_ABS")!=std::string::npos);
    assert(helper.find("UI_SET_ABSBIT,ABS_X")!=std::string::npos);
    assert(helper.find("UI_SET_ABSBIT,ABS_Y")!=std::string::npos);
    assert(helper.find("INPUT_PROP_DIRECT")!=std::string::npos);
    assert(helper.find("INPUT_PROP_POINTER")!=std::string::npos);
    assert(helper.find("t==\"POINTER\"")!=std::string::npos);
    assert(helper.find("OPAL Remote Absolute Pointer")!=std::string::npos);

    auto client=read_file("src/client.cpp");
    assert(client.find("event.type==KeyPress||event.type==KeyRelease")!=std::string::npos);
    assert(client.find("XIQueryDevice")==std::string::npos);
    assert(client.find("XQueryPointer")!=std::string::npos);
    assert(client.find("video_pointer_command")!=std::string::npos);
    assert(client.find("session.remote_width()")!=std::string::npos);
    assert(client.find("session.remote_height()")!=std::string::npos);
    assert(client.find("PointerMotionMask")!=std::string::npos);
    assert(client.find("XISetMask(mask,XI_RawMotion)")==std::string::npos);
    assert(client.find("XWarpPointer")==std::string::npos);
    assert(client.find("ClientControlChord::ReleaseCapture")!=std::string::npos);
    assert(client.find("wait_for_reacquire_click")!=std::string::npos);
    assert(client.find("session.presentation_window()")!=std::string::npos);
    assert(client.find("XUngrabKeyboard")!=std::string::npos);
    assert(client.find("XUngrabPointer")!=std::string::npos);
    assert(client.find("click the OPAL screen to capture again")!=std::string::npos);

    auto session_header=read_file("include/opal/session.hpp");
    auto session_source=read_file("src/session.cpp");
    assert(session_header.find("presentation_window() const")!=std::string::npos);
    assert(session_source.find("receiver->presentation_window()")!=std::string::npos);

    auto host=read_file("src/host.cpp");
    assert(host.find("line.rfind(\"POINTER \",0)==0")!=std::string::npos);
    assert(host.find("DisplayWidth")!=std::string::npos);
    assert(host.find("DisplayHeight")!=std::string::npos);
    assert(host.find("CHALLENGE OPAL3 "+std::string("\"+challenge"))!=std::string::npos);

    auto makefile=read_file("Makefile");
    assert(makefile.find("-lXi")!=std::string::npos);
    auto ci=read_file(".github/workflows/ci.yml");
    assert(ci.find("libxi-dev")!=std::string::npos);
}
