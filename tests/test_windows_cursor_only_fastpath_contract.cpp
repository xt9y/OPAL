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
    const auto source = read_all("src/platform/windows/capture_backend.cpp");

    const auto composite_guard = source.find("if (!desktop_updated && output.ready)");
    const auto composite_copy = source.find("CopyResource(output.latest_texture, texture)", composite_guard);
    assert(composite_guard != std::string::npos);
    assert(composite_copy != std::string::npos);
    assert(source.find("return AcquireResult::PointerUpdated;", composite_guard) < composite_copy);

    const auto next_single = source.find("bool next_single");
    const auto single_guard = source.find("if (!desktop_updated && output.ready)", composite_guard + 1);
    const auto single_compose = source.find("return compose(frame, output.capture_us, output.timestamp_quality);", single_guard);
    const auto single_copy = source.find("CopyResource(output.latest_texture, texture)", single_guard);
    assert(next_single != std::string::npos);
    assert(single_guard > next_single);
    assert(single_compose != std::string::npos);
    assert(single_copy != std::string::npos);
    assert(single_compose < single_copy);

    return 0;
}
