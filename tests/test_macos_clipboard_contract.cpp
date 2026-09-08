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
    const auto text = read_all("src/platform/macos/clipboard_shim.mm");
    assert(text.find("NSPasteboard") != std::string::npos);
    assert(text.find("NSPasteboardTypeString") != std::string::npos);
    assert(text.find("clearContents") != std::string::npos);
    assert(text.find("setString") != std::string::npos);
    assert(text.find("wl-copy") == std::string::npos);
    assert(text.find("wl-paste") == std::string::npos);
    return 0;
}
