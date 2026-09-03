#include <cassert>
#include <fstream>
#include <iterator>
#include <string>

static std::string read_all(const char *path){
    std::ifstream in(path);assert(in.good());
    return std::string(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());
}

int main(){
    const auto source=read_all("src/video_receiver.cpp");
    assert(source.find("media_thread")!=std::string::npos);
    assert(source.find("media_cv")!=std::string::npos);
    assert(source.find("recv_datagrams_batch")!=std::string::npos);
    assert(source.find("std::optional<MediaItem> video_frame")!=std::string::npos);
    assert(source.find("std::optional<MediaItem> video_config")!=std::string::npos);
    assert(source.find("std::deque<MediaItem>")==std::string::npos);
    assert(source.find("video_frame.reset()")!=std::string::npos);
    assert(source.find("request_idr_rx")!=std::string::npos);
    return 0;
}
