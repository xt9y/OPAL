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
    const auto text = read_all("src/platform/macos/video_capture.cpp");
    const auto poll = text.find("bool poll(EncodedMediaView& view, int video_wait_ms)");
    assert(poll != std::string::npos);
    const auto audio = text.find("audio->next(storage, 0)", poll);
    const auto video = text.find("video->next(storage, video_wait_ms)", poll);
    assert(audio != std::string::npos && video != std::string::npos);
    assert(audio < video);
    assert(text.find("Video capture itself is\n        // latest-only") != std::string::npos);
    return 0;
}
