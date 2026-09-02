#include <opal/input.hpp>
#include <cassert>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(){
    assert(opal::linux_keycode_from_x11(9)==1);
    assert(opal::linux_keycode_from_x11(108)==100);
    assert(opal::linux_keycode_from_x11(0)==0);
    assert(opal::linux_keycode_from_x11(7)==0);
    assert(opal::linux_keycode_from_x11(8)==0);
    assert(opal::linux_keycode_from_x11(100000)==0);

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

    std::ifstream f("src/input_helper.cpp");
    std::string source((std::istreambuf_iterator<char>(f)),{});
    assert(source.find("KEY_MAX")!=std::string::npos);
    assert(source.find("k<256")==std::string::npos);
    assert(source.find("i<256")==std::string::npos);
}
