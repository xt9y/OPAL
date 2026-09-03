#include <opal/crypto.hpp>
#include <cassert>
#include <fstream>
#include <iterator>
#include <string>

static std::string read_all(const char*path){
    std::ifstream in(path);
    assert(in.good());
    return std::string((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
}

int main(){
    const auto code=opal::pairing_code();
    assert(code.size()==19);
    assert(code[4]=='-'&&code[9]=='-'&&code[14]=='-');

    const auto input=read_all("src/input_helper.cpp");
    assert(input.find("O_CLOEXEC")!=std::string::npos);
    assert(input.find("O_NONBLOCK")==std::string::npos);

    const auto wake=read_all("src/wake.cpp");
    assert(wake.find("SO_RCVTIMEO")!=std::string::npos);
    assert(wake.find("SO_SNDTIMEO")!=std::string::npos);
    assert(wake.find("secret.empty()")!=std::string::npos);

    const auto client=read_all("src/client.cpp");
    assert(client.find("i<100")!=std::string::npos);
    assert(client.find("ffplay")==std::string::npos);
    assert(client.find("47991")==std::string::npos);

    const auto host=read_all("src/host.cpp");
    assert(host.find("sanitize_label")!=std::string::npos);
    assert(host.find("CHALLENGE OPAL2 ")!=std::string::npos);
    assert(host.find("negotiate_host_direct_video")!=std::string::npos);
    assert(host.find("DIRECT_RECEIVER_READY")!=std::string::npos);
    assert(host.find("DIRECT_MEDIA_READY")!=std::string::npos);
    assert(host.find("video_session_active")==std::string::npos);
    assert(host.find("47991")==std::string::npos);
    assert(host.find("listen_tcp(static_cast<uint16_t>(cp)")!=std::string::npos);

    const auto session=read_all("src/session.cpp");
    assert(session.find("negotiate_client_direct_video")!=std::string::npos);
    assert(session.find("VIDEO_PROFILE ")!=std::string::npos);
    assert(session.find("DIRECT_RECEIVER_READY ")!=std::string::npos);
    assert(session.find("ffplay")==std::string::npos);
    assert(session.find("open_video")==std::string::npos);
    assert(session.find("video_port")==std::string::npos);
    assert(session.find("control queue overflow")!=std::string::npos);
    assert(session.find("incompatible OPAL host protocol")!=std::string::npos);
    assert(session.find("normalize_pairing_code")!=std::string::npos);

    const auto session_header=read_all("include/opal/session.hpp");
    assert(session_header.find("video_port")==std::string::npos);
    assert(session_header.find("video_token")==std::string::npos);

    const auto host_header=read_all("include/opal/host.hpp");
    assert(host_header.find("video_backpressure_timeout_ms")==std::string::npos);

    const auto media=read_all("src/media.cpp");
    const auto media_header=read_all("include/opal/media.hpp");
    assert(media.find("video_request_line")==std::string::npos);
    assert(media.find("parse_video_request_line")==std::string::npos);
    assert(media.find("video_player_write_timeout_ms")==std::string::npos);
    assert(media_header.find("video_request_line")==std::string::npos);

    const auto packet_header=read_all("include/opal/video_packet.hpp");
    assert(packet_header.find("Keepalive=6")!=std::string::npos);

    const auto tunnel=read_all("src/tunnel.cpp");
    assert(tunnel.find("zrok2-control-only")!=std::string::npos);
    assert(tunnel.find("connection_code=\"opal:\"+control")!=std::string::npos);
    assert(tunnel.find("video_pid")==std::string::npos);
    assert(tunnel.find("opal-vid-")==std::string::npos);
    assert(tunnel.find("--bind\",\"127.0.0.1:47991") == std::string::npos);
    assert(tunnel.find("\"127.0.0.1:47991\"},true") == std::string::npos);

    const auto service=read_all("systemd/opal-host.service");
    assert(service.find("NoNewPrivileges=true")!=std::string::npos);
    assert(service.find("PrivateTmp=true")!=std::string::npos);
    assert(service.find("RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6")!=std::string::npos);
    assert(service.find("UMask=0077")!=std::string::npos);

    const auto readme=read_all("README.md");
    assert(readme.find("mktemp -d")!=std::string::npos);
    assert(readme.find("direct encrypted UDP")!=std::string::npos);
    assert(readme.find("No zrok-video")!=std::string::npos||readme.find("no zrok-video")!=std::string::npos);
    assert(readme.find("/tmp/zrok2.tar.gz")==std::string::npos);

    const auto ci=read_all(".github/workflows/ci.yml");
    assert(ci.find("workflow_dispatch")!=std::string::npos);
    assert(ci.find("\n  push:")==std::string::npos);
    assert(ci.find("sanitize")!=std::string::npos);
    assert(ci.find("stress")!=std::string::npos);
    return 0;
}
