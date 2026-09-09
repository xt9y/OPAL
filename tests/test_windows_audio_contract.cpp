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
    const auto source = read_all("src/platform/windows/audio_capture_backend.cpp");
    assert(source.find("IMMDeviceEnumerator") != std::string::npos);
    assert(source.find("eRender") != std::string::npos);
    assert(source.find("IAudioClient") != std::string::npos);
    assert(source.find("IAudioCaptureClient") != std::string::npos);
    assert(source.find("AUDCLNT_STREAMFLAGS_LOOPBACK") != std::string::npos);
    assert(source.find("AUDCLNT_STREAMFLAGS_EVENTCALLBACK") != std::string::npos);
    assert(source.find("SetEventHandle") != std::string::npos);
    assert(source.find("GetBuffer") != std::string::npos);
    assert(source.find("AV_CODEC_ID_AAC") != std::string::npos);
    assert(source.find("av_audio_fifo") != std::string::npos);
    assert(source.find("popen") == std::string::npos);
    assert(source.find("ffmpeg ") == std::string::npos);
    return 0;
}
