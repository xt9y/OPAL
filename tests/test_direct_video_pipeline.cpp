#include <opal/direct_video_session.hpp>
#include <opal/udp_transport.hpp>
#include <opal/video_receiver.hpp>
#include <opal/video_sender.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

opal::DirectVideoPath make_path(opal::UdpSocket socket,std::uint16_t peer_port,bool sender,std::uint32_t generation){
    opal::DirectVideoPath path;path.socket=socket;path.generation=generation;
    path.session_id=0x1122334455667788ULL+generation;
    assert(opal::resolve_udp_endpoint("::1",peer_port,path.peer,path.peer_len));
    for(std::size_t i=0;i<32;++i){
        if(sender)path.keys.send_key[i]=static_cast<std::uint8_t>(i+1);
        else path.keys.recv_key[i]=static_cast<std::uint8_t>(i+1);
    }
    for(std::size_t i=0;i<12;++i){
        if(sender)path.keys.send_nonce_base[i]=static_cast<std::uint8_t>(0x80+i);
        else path.keys.recv_nonce_base[i]=static_cast<std::uint8_t>(0x80+i);
    }
    return path;
}

void run_case(int dropped_fragments,bool expect_idr,std::uint32_t generation){
    setenv("OPAL_TEST_DROP_FRAGMENTS",std::to_string(dropped_fragments).c_str(),1);
    auto sender_socket=opal::open_udp_socket(),receiver_socket=opal::open_udp_socket();
    assert(sender_socket.fd>=0&&receiver_socket.fd>=0);
    auto sender_path=make_path(sender_socket,receiver_socket.local_port,true,generation);
    auto receiver_path=make_path(receiver_socket,sender_socket.local_port,false,generation);

    opal::VideoSender sender;opal::VideoReceiver receiver;
    std::atomic<int> idr_requests{0};
    assert(receiver.start(std::move(receiver_path),[&](const std::string &line){
        if(line.rfind("REQUEST_IDR ",0)==0)++idr_requests;
        assert(sender.handle_control_line(line));
    }));
    assert(sender.start(std::move(sender_path),{320,180,60},false,[&](const std::string &line){
        assert(receiver.handle_control_line(line));
    }));

    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
    while(!receiver.media_started()&&std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(receiver.media_started());
    assert(receiver.presentation_window()!=0);
    assert(sender.queued_frames()<=2&&sender.queued_bytes()<=16u*1024u*1024u);
    assert(opal::kVideoMaxDatagramBytes==1200);
    if(expect_idr)assert(idr_requests.load()>=1);
    else assert(idr_requests.load()==0);

    sender.stop();receiver.stop();
}

}

int main(){
    setenv("OPAL_VIDEO_WINDOWED","1",1);
    setenv("OPAL_CAPTURE_CMD","ffmpeg -hide_banner -loglevel error -re -f lavfi -i testsrc=size=320x180:rate=60 -frames:v 180 -pix_fmt yuv420p -c:v libx264 -preset ultrafast -tune zerolatency -bf 0 -g 15 -keyint_min 15 -sc_threshold 0 -an -f flv pipe:1",1);

    run_case(1,false,9);  // one missing fragment is reconstructed by XOR FEC
    run_case(2,true,10);  // two missing fragments require fresh IDR recovery

    unsetenv("OPAL_TEST_DROP_FRAGMENTS");
    unsetenv("OPAL_CAPTURE_CMD");
    unsetenv("OPAL_VIDEO_WINDOWED");
    return 0;
}
