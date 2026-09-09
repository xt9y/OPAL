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
    const auto source = read_all("src/platform/windows/clipboard_shim.cpp");
    assert(source.find("CF_UNICODETEXT") != std::string::npos);
    assert(source.find("OpenClipboard") != std::string::npos);
    assert(source.find("GetClipboardData") != std::string::npos);
    assert(source.find("SetClipboardData") != std::string::npos);
    assert(source.find("MultiByteToWideChar") != std::string::npos);
    assert(source.find("WideCharToMultiByte") != std::string::npos);
    assert(source.find("opal_windows_get_clipboard_text") != std::string::npos);
    assert(source.find("opal_windows_set_clipboard_text") != std::string::npos);
    return 0;
}
