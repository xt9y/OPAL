#pragma once
#include <opal/udp_transport.hpp>
#include <opal/video_crypto.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <openssl/ssl.h>

namespace opal {
struct DirectVideoPath {
    UdpSocket socket;
    sockaddr_storage peer{};
    socklen_t peer_len=0;
    VideoKeys keys;
    std::uint64_t session_id=0;
    DirectVideoPath()=default;
    ~DirectVideoPath();
    DirectVideoPath(const DirectVideoPath&)=delete;
    DirectVideoPath& operator=(const DirectVideoPath&)=delete;
    DirectVideoPath(DirectVideoPath&&) noexcept;
    DirectVideoPath& operator=(DirectVideoPath&&) noexcept;
};

using ControlSend=std::function<bool(const std::string&,int)>;
using ControlRead=std::function<bool(std::string&,int)>;

const char* direct_video_unavailable_error();
bool negotiate_client_direct_video(SSL*,const std::string& session_token,
    const std::string& client_pub,const std::string& host_fp,std::uint32_t generation,
    const std::vector<StunEndpoint>&,ControlSend,ControlRead,DirectVideoPath&,std::string&,int deadline_ms=5000);
bool negotiate_host_direct_video(SSL*,const std::string& session_token,
    const std::string& client_pub,const std::string& host_fp,std::uint32_t generation,
    const std::vector<StunEndpoint>&,ControlSend,ControlRead,DirectVideoPath&,std::string&,int deadline_ms=5000);
}
