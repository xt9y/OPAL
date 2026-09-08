#include <cassert>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include <opal/input_wire.hpp>
#include <opal/platform.hpp>
#include <opal/platform_error.hpp>

static std::string read_all(const char *path)
{
    std::ifstream in(path);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

int main()
{
    static_assert(opal::wire_key::Esc == 1);
    static_assert(opal::wire_key::Q == 16);
    static_assert(opal::wire_key::W == 17);
    static_assert(opal::wire_key::LeftCtrl == 29);
    static_assert(opal::wire_key::A == 30);
    static_assert(opal::wire_key::LeftShift == 42);
    static_assert(opal::wire_key::LeftAlt == 56);
    static_assert(opal::wire_key::RightCtrl == 97);
    static_assert(opal::wire_key::RightAlt == 100);
    static_assert(opal::wire_key::LeftMeta == 125);
    static_assert(opal::wire_key::RightMeta == 126);

    static_assert(opal::platform_component_name(opal::PlatformComponent::Capture) == std::string_view("capture"));
    static_assert(opal::platform_component_name(opal::PlatformComponent::AudioCapture) == std::string_view("audio-capture"));
    static_assert(opal::platform_failure_name(opal::PlatformFailure::PermissionDenied) == std::string_view("permission-denied"));
    static_assert(opal::platform_failure_name(opal::PlatformFailure::DependencyMissing) == std::string_view("dependency-missing"));

    opal::PlatformError error{};
    assert(!error);
    error.component = opal::PlatformComponent::Capture;
    error.failure = opal::PlatformFailure::PermissionDenied;
    error.message = "screen recording permission denied";
    assert(error);
    assert(!error.fallback_possible);

    for (const char *path : {
             "include/opal/input.hpp",
             "include/opal/input_wire.hpp",
             "include/opal/platform.hpp",
             "include/opal/platform_error.hpp"}) {
        const auto text = read_all(path);
        assert(text.find("<linux/") == std::string::npos);
        assert(text.find("sockaddr_storage") == std::string::npos);
        assert(text.find("AppKit/") == std::string::npos);
        assert(text.find("CoreGraphics/") == std::string::npos);
        assert(text.find("CVPixelBufferRef") == std::string::npos);
    }

    return 0;
}
