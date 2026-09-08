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
    assert(text.find("SCStreamDelegate") != std::string::npos);
    assert(text.find("didStopWithError") != std::string::npos);
    assert(text.find("delegate:output_") != std::string::npos);
    assert(text.find("std::atomic<bool> running_") != std::string::npos);
    assert(text.find("capturesAudio = YES") != std::string::npos);
    assert(text.find("sampleRate = 48000") != std::string::npos);
    assert(text.find("channelCount = 2") != std::string::npos);
    assert(text.find("CGMainDisplayID") != std::string::npos);
    assert(text.find("candidate.displayID == main_id") != std::string::npos);
    assert(text.find("synchronizationClock") != std::string::npos);
    assert(text.find("CMClockGetTime") != std::string::npos);
    assert(text.find("AVAudioFifo") != std::string::npos);
    assert(text.find("avcodec_find_encoder(AV_CODEC_ID_AAC)") != std::string::npos);
    assert(text.find("AV_CODEC_FLAG_GLOBAL_HEADER") != std::string::npos);
    assert(text.find("avcodec_get_supported_config") != std::string::npos);
    assert(text.find("swr_alloc_set_opts2") != std::string::npos);
    assert(text.find("AudioSpecificConfig") != std::string::npos);
    assert(text.find("fifo_capture_us_") != std::string::npos);
    assert(text.find("fifo_samples_before") != std::string::npos);
    assert(text.find("unit.capture_time_us = fifo_capture_us_") != std::string::npos);
    assert(text.find("dispatch_release(content_sem)") != std::string::npos);
    assert(text.find("dispatch_release(start_sem)") != std::string::npos);
    assert(text.find("dispatch_release(stop_sem)") != std::string::npos);
    assert(text.find("dispatch_release(queue_)") != std::string::npos);
    assert(text.find("#define AVMediaType OPAL_FFmpegAVMediaType") != std::string::npos);
    assert(text.find("#undef AVMediaType") != std::string::npos);
    const auto define_pos = text.find("#define AVMediaType OPAL_FFmpegAVMediaType");
    const auto avcodec_pos = text.find("#include <libavcodec/avcodec.h>");
    const auto undef_pos = text.find("#undef AVMediaType");
    assert(define_pos < avcodec_pos && avcodec_pos < undef_pos);
    assert(text.find("ffmpeg -") == std::string::npos);
    return 0;
}
