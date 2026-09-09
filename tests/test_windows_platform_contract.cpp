#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <opal/platform.hpp>

static std::string read_all(const std::filesystem::path& path)
{
    std::ifstream in(path);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

int main()
{
    static_assert(opal::platform_name(opal::PlatformKind::Windows) == std::string_view("windows"));

    const std::vector<std::string> forbidden = {
        "<windows.h>",
        "<winsock2.h>",
        "HANDLE",
        "SOCKET",
        "HWND",
        "HRESULT",
        "ID3D11",
        "IDXGI",
        "IMFTransform",
        "IMMDevice"
    };

    for (const auto& entry : std::filesystem::recursive_directory_iterator("include/opal")) {
        if (!entry.is_regular_file() || entry.path().extension() != ".hpp") continue;
        const auto text = read_all(entry.path());
        for (const auto& token : forbidden) assert(text.find(token) == std::string::npos);
    }

    return 0;
}
