#pragma once

#include <opal/udp_transport.hpp>
#include <opal/video_crypto.hpp>
#include <cstdint>
#include <utility>

namespace opal {

struct DirectVideoPath {
    UdpSocket socket;
    sockaddr_storage peer{};
    socklen_t peer_len=0;
    VideoKeys keys;
    std::uint64_t session_id=0;
    std::uint32_t generation=0;

    DirectVideoPath()=default;
    ~DirectVideoPath(){close_udp_socket(socket);}
    DirectVideoPath(const DirectVideoPath&)=delete;
    DirectVideoPath& operator=(const DirectVideoPath&)=delete;
    DirectVideoPath(DirectVideoPath&&other) noexcept{*this=std::move(other);}
    DirectVideoPath& operator=(DirectVideoPath&&other) noexcept{
        if(this!=&other){
            close_udp_socket(socket);
            socket=other.socket;other.socket={};
            peer=other.peer;peer_len=other.peer_len;other.peer_len=0;
            keys=other.keys;
            session_id=other.session_id;other.session_id=0;
            generation=other.generation;other.generation=0;
        }
        return *this;
    }
};

}
