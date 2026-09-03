#pragma once
#include <opal/video_path.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <openssl/ssl.h>

namespace opal {

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
