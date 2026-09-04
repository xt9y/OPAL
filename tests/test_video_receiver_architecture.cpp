#include <cassert>
#include <fstream>
#include <iterator>
#include <string>

static std::string read_all(const char *path){std::ifstream in(path);assert(in.good());return std::string(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());}

int main(){
    const auto source=read_all("src/video_receiver.cpp");const auto header=read_all("include/opal/video_receiver.hpp");
    assert(source.find("media_thread")!=std::string::npos);
    assert(source.find("media_cv")!=std::string::npos);
    assert(source.find("recv_datagrams_batch")!=std::string::npos);
    assert(source.find("kVideoDecodeBacklogCapacity=8")!=std::string::npos);
    assert(source.find("std::array<std::optional<MediaItem>,kVideoDecodeBacklogCapacity>video_frames")!=std::string::npos);
    assert(source.find("clear_video_backlog")!=std::string::npos);
    assert(source.find("push_video_backlog")!=std::string::npos);
    assert(source.find("pop_video_backlog")!=std::string::npos);
    assert(source.find("decode-backlog")!=std::string::npos);
    assert(source.find("publish_latest")!=std::string::npos);
    assert(source.find("av_frame_clone")!=std::string::npos);
    assert(source.find("DecodedVideoFrame latest_frame")!=std::string::npos);
    assert(source.find("take_latest")!=std::string::npos);
    assert(source.find("VideoPresenter")==std::string::npos);
    assert(source.find("present_borrowed")==std::string::npos);
    assert(header.find("take_latest_video")!=std::string::npos);
    assert(header.find("presentation_window")==std::string::npos);
    return 0;
}
