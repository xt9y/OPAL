#include <opal/input.hpp>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iterator>
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

    // Raw XI2 deltas are already the physical device counts. OPAL must not
    // reinterpret XI2 valuator resolution as a request to change the mouse's
    // count rate: that makes the remote mouse intrinsically faster/slower than
    // the source. Resolution metadata therefore never changes the default 1:1
    // relative motion path.
    assert(near(opal::mouse_normalization_scale(0),1.0));
    assert(near(opal::mouse_normalization_scale(1000),1.0));
    assert(near(opal::mouse_normalization_scale(5000),1.0));
    assert(near(opal::mouse_normalization_scale(125984),1.0));
    assert(near(opal::mouse_normalization_scale(400000),1.0));

    assert(near(opal::clamp_mouse_sensitivity(0.01),0.1));
    assert(near(opal::clamp_mouse_sensitivity(1.0),1.0));
    assert(near(opal::clamp_mouse_sensitivity(9.0),4.0));

    assert(opal::normalized_motion_command(32.0,-16.0,125984,125984)=="MOUSE 32 -16");
    assert(opal::normalized_motion_command(8.0,-4.0,31496,31496)=="MOUSE 8 -4");
    assert(opal::normalized_motion_command(12.0,-7.0,0,0)=="MOUSE 12 -7");
    assert(opal::normalized_motion_command(32.0,-16.0,125984,125984,2.0)=="MOUSE 64 -32");

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

    // Exact cursor parity requires absolute host injection. Relative uinput
    // motion is intentionally processed by the host input stack and may be
    // accelerated again, so even 1:1 client deltas cannot guarantee parity.
    auto helper=read_file("src/input_helper.cpp");
    assert(helper.find("KEY_MAX")!=std::string::npos);
    assert(helper.find("k<256")==std::string::npos);
    assert(helper.find("i<256")==std::string::npos);
    assert(helper.find("UI_SET_EVBIT,EV_ABS")!=std::string::npos);
    assert(helper.find("UI_SET_ABSBIT,ABS_X")!=std::string::npos);
    assert(helper.find("UI_SET_ABSBIT,ABS_Y")!=std::string::npos);
    assert(helper.find("t==\"POINTER\"")!=std::string::npos);

    auto client=read_file("src/client.cpp");
    assert(client.find("event.type==KeyPress||event.type==KeyRelease")!=std::string::npos);
    assert(client.find("XIQueryDevice")==std::string::npos);
    assert(client.find("XQueryPointer")!=std::string::npos);
    assert(client.find("absolute_pointer_command")!=std::string::npos);

    auto host=read_file("src/host.cpp");
    assert(host.find("line.rfind(\"POINTER \",0)==0")!=std::string::npos);

    auto makefile=read_file("Makefile");
    assert(makefile.find("-lXi")!=std::string::npos);
    auto ci=read_file(".github/workflows/ci.yml");
    assert(ci.find("libxi-dev")!=std::string::npos);
}
