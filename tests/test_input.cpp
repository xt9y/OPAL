#include <opal/input.hpp>
#include <cassert>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static std::string read_file(const char*path){std::ifstream f(path);return std::string((std::istreambuf_iterator<char>(f)),{});}

int main(){
    assert(opal::linux_keycode_from_x11(9)==1);
    assert(opal::linux_keycode_from_x11(108)==100);
    assert(opal::linux_keycode_from_x11(0)==0);
    assert(opal::linux_keycode_from_x11(7)==0);
    assert(opal::linux_keycode_from_x11(8)==0);
    assert(opal::linux_keycode_from_x11(100000)==0);
    assert(opal::raw_motion_command(12.0,-7.0)=="MOUSE 12 -7");

    // XInput2 reports valuator resolution in counts/metre. Normalize physical
    // mouse motion to the 1000-DPI scale libinput assumes for the OPAL uinput
    // mouse so client and host DPI do not multiply cursor speed.
    assert(opal::normalized_motion_command(32.0,-16.0,125984,125984)=="MOUSE 10 -5"); // 3200 DPI source
    assert(opal::normalized_motion_command(8.0,-4.0,31496,31496)=="MOUSE 10 -5");    // 800 DPI source
    assert(opal::normalized_motion_command(12.0,-7.0,0,0)=="MOUSE 12 -7");           // unknown DPI fallback

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

    // XGrabKeyboard delivers KeyPress/KeyRelease to the grabbing client. The
    // XI2 loop must consume those core events instead of discarding everything
    // that is not GenericEvent.
    auto client=read_file("src/client.cpp");
    assert(client.find("event.type==KeyPress||event.type==KeyRelease")!=std::string::npos);
    assert(client.find("XIQueryDevice")!=std::string::npos);

    auto makefile=read_file("Makefile");
    assert(makefile.find("-lXi")!=std::string::npos);
    auto ci=read_file(".github/workflows/ci.yml");
    assert(ci.find("libxi-dev")!=std::string::npos);
}
