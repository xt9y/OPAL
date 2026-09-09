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

static std::string section(const std::string& text, const char* begin, const char* end)
{
    const auto first = text.find(begin);
    assert(first != std::string::npos);
    const auto last = text.find(end, first);
    assert(last != std::string::npos && last > first);
    return text.substr(first, last - first);
}

int main()
{
    const auto text = read_all("src/platform/windows/audio_capture_backend.cpp");

    assert(text.find("GetBufferSize(&buffer_frame_count_)") != std::string::npos);
    assert(text.find("AVFrame* encode_frame_ = nullptr") != std::string::npos);
    assert(text.find("std::uint8_t** resample_data_ = nullptr") != std::string::npos);
    assert(text.find("int resample_capacity_ = 0") != std::string::npos);
    assert(text.find("av_audio_fifo_space(fifo_)") != std::string::npos);
    assert(text.find("std::fill_n(silence_.data()") != std::string::npos);

    const auto ingest = section(text, "bool ingest_pcm(", "bool encode_one(");
    assert(ingest.find("av_samples_alloc_array_and_samples") == std::string::npos);
    assert(ingest.find("av_freep") == std::string::npos);

    const auto encode = section(text, "bool encode_one(", "void reset_encoder(");
    assert(encode.find("av_frame_alloc") == std::string::npos);
    assert(encode.find("av_frame_get_buffer") == std::string::npos);
    assert(encode.find("av_frame_free") == std::string::npos);

    return 0;
}
