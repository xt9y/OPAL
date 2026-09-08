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
    const auto text = read_all("src/platform/macos/audio_capture_backend.mm");
    assert(text.find("SCStreamOutputTypeAudio") != std::string::npos);
    assert(text.find("capturesAudio = YES") != std::string::npos);
    assert(text.find("sampleRate = 48000") != std::string::npos);
    assert(text.find("channelCount = 2") != std::string::npos);
    assert(text.find("CGMainDisplayID") != std::string::npos);
    assert(text.find("candidate.displayID == main_id") != std::string::npos);
    assert(text.find("AVAudioFifo") != std::string::npos);
    assert(text.find("avcodec_find_encoder(AV_CODEC_ID_AAC)") != std::string::npos);
    assert(text.find("AV_CODEC_FLAG_GLOBAL_HEADER") != std::string::npos);
    assert(text.find("avcodec_get_supported_config") != std::string::npos);
    assert(text.find("swr_alloc_set_opts2") != std::string::npos);
    assert(text.find("AudioSpecificConfig") != std::string::npos);
    assert(text.find("ffmpeg -") == std::string::npos);
    return 0;
}
