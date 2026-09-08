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
    const auto sender = read_all("src/video_sender.cpp");
    assert(sender.find("capture.set_bitrate(next)") != std::string::npos);
    assert(sender.find("capture.request_idr()") != std::string::npos);
    assert(sender.find("encoder_idr_request_sent") != std::string::npos);
    assert(sender.find("send_datagram_result(owned->socket,owned->peer,wire)") != std::string::npos);
    assert(sender.find("send_datagrams_batch(owned->socket,owned->peer,wires)") != std::string::npos);
    assert(sender.find("owned->peer_len") == std::string::npos);
    return 0;
}
