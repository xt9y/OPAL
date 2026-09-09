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
    assert(source.find("AddClipboardFormatListener") != std::string::npos);
    assert(source.find("RemoveClipboardFormatListener") != std::string::npos);
    assert(source.find("WM_CLIPBOARDUPDATE") != std::string::npos);
    assert(source.find("HWND_MESSAGE") != std::string::npos);
    assert(source.find("generation_.fetch_add") != std::string::npos);
    assert(source.find("cached_text") != std::string::npos);
    assert(source.find("opal_windows_clipboard_generation") != std::string::npos);
    assert(source.find("opal_windows_get_clipboard_text") != std::string::npos);
    assert(source.find("opal_windows_set_clipboard_text") != std::string::npos);
    return 0;
}
