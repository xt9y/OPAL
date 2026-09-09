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
    const auto text = read_all("src/platform/windows/system_backend.cpp");

    assert(text.find("std::filesystem::path(std::wstring(buffer.data(), length))") != std::string::npos);
    assert(text.find("wide_to_utf8") == std::string::npos);
    assert(text.find("utf8_to_wide(executable_path.string())") == std::string::npos);
    assert(text.find("utf8_to_wide(executable_path.parent_path().string())") == std::string::npos);
    assert(text.find("executable_path.native()") != std::string::npos);
    assert(text.find("executable_path.parent_path().native()") != std::string::npos);
    return 0;
}
