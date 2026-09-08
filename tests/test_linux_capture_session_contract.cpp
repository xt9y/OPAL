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
    const auto source = read_all("src/pipewire_capture.cpp");
    const auto header = read_all("include/opal/pipewire_capture.hpp");

    assert(source.find("class PersistentPipeWireHub") != std::string::npos);
    assert(source.find("XDP_SCREENCAST_FLAG_MULTIPLE") != std::string::npos);
    assert(source.find("g_variant_n_children(streams)") != std::string::npos);
    assert(source.find("for (gsize i = 0; i < stream_count; ++i)") != std::string::npos);
    assert(source.find("portal-session.token.tmp") == std::string::npos);
    assert(source.find("rename(temp.c_str(), path.c_str())") != std::string::npos);
    assert(source.find("fsync(fd)") != std::string::npos);
    assert(source.find("xdp_session_get_restore_token") != std::string::npos);
    assert(source.find("build_composite_layout") != std::string::npos);
    assert(source.find("sws_scale") != std::string::npos);
    assert(header.find("native_pipewire_prepare") != std::string::npos);
    assert(header.find("native_pipewire_layout") != std::string::npos);

    const auto stop = source.find("void NativePipeWireVideoCapture::stop()");
    assert(stop != std::string::npos);
    const auto stop_tail = source.substr(stop);
    assert(stop_tail.find("xdp_session_close") == std::string::npos);
    assert(stop_tail.find("pw_stream_destroy") == std::string::npos);

    return 0;
}
