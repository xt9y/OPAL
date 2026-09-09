#include <cassert>
#include <fstream>
#include <sstream>
#include <string>

static std::string read_all(const char* path)
{
    std::ifstream in(path);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

int main()
{
    const auto source = read_all("src/platform/windows/input_helper.cpp");
    assert(source.find("SendInput") != std::string::npos);
    assert(source.find("KEYEVENTF_SCANCODE") != std::string::npos);
    assert(source.find("KEYEVENTF_EXTENDEDKEY") != std::string::npos);
    assert(source.find("MOUSEEVENTF_MOVE") != std::string::npos);
    assert(source.find("MOUSEEVENTF_ABSOLUTE") != std::string::npos);
    assert(source.find("MOUSEEVENTF_WHEEL") != std::string::npos);
    assert(source.find("decode_input_record") != std::string::npos);
    assert(source.find("parse_input_command") != std::string::npos);
    assert(source.find("ReadFile") != std::string::npos);
    assert(source.find("SetCursorPos") == std::string::npos);
    assert(source.find("keybd_event") == std::string::npos);
    assert(source.find("mouse_event") == std::string::npos);
    return 0;
}
