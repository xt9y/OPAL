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

    // XI2 resolution is counts/metre. Synthetic XWayland values around 1000
    // are not physical DPI metadata and must never create a ~39x multiplier.
    assert(near(opal::mouse_normalization_scale(0),1.0));
    assert(near(opal::mouse_normalization_scale(1000),1.0));
    assert(near(opal::mouse_normalization_scale(4999),1.0));
    assert(near(opal::mouse_normalization_scale(400001),1.0));

    // Physically plausible metadata is normalized toward 1000 DPI and the
    // automatic multiplier is bounded even at the edge of the accepted range.
    assert(near(opal::mouse_normalization_scale(5000),4.0));
    assert(near(opal::mouse_normalization_scale(400000),0.25));
    assert(opal::mouse_normalization_scale(125984)>0.30);
    assert(opal::mouse_normalization_scale(125984)<0.33);
    assert(near(opal::clamp_mouse_sensitivity(0.01),0.1));
    assert(near(opal::clamp_mouse_sensitivity(1.0),1.0));
    assert(near(opal::clamp_mouse_sensitivity(9.0),4.0));

    assert(opal::normalized_motion_command(32.0,-16.0,125984,125984)=="MOUSE 10 -5"); // 3200 DPI source
    assert(opal::normalized_motion_command(8.0,-4.0,31496,31496)=="MOUSE 10 -5");    // 800 DPI source
    assert(opal::normalized_motion_command(12.0,-7.0,0,0)=="MOUSE 12 -7");           // unknown DPI fallback
    assert(opal::normalized_motion_command(12.0,-7.0,1000,1000)=="MOUSE 12 -7");    // synthetic XWayland fallback
    assert(opal::normalized_motion_command(32.0,-16.0,125984,125984,2.0)=="MOUSE 20 -10");

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

    auto helper=read_file("src/input_helper.cpp");
    assert(helper.find("KEY_MAX")!=std::string::npos);
    assert(helper.find("k<256")==std::string::npos);
    assert(helper.find("i<256")==std::string::npos);

    auto client=read_file("src/client.cpp");
    assert(client.find("event.type==KeyPress||event.type==KeyRelease")!=std::string::npos);
    assert(client.find("XIQueryDevice")!=std::string::npos);

    auto makefile=read_file("Makefile");
    assert(makefile.find("-lXi")!=std::string::npos);
    auto ci=read_file(".github/workflows/ci.yml");
    assert(ci.find("libxi-dev")!=std::string::npos);
}
