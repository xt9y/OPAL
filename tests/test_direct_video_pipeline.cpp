#include <opal/video_path.hpp>
#include <opal/udp_transport.hpp>
#include <opal/video_packet.hpp>
#include <opal/video_receiver.hpp>
#include <opal/video_sender.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {
using namespace std::chrono_literals;

std::string read_all(const std::filesystem::path&path){std::ifstream in(path);return std::string((std::istreambuf_iterator<char>(in)),{});}
std::string make_capture_script(const std::filesystem::path&root){auto path=root/"capture.sh";std::ofstream out(path);out<<R"SH(#!/bin/sh
exec ffmpeg -hide_banner -loglevel error -re -f lavfi -i testsrc=size=320x180:rate=60 -f lavfi -i anullsrc=r=48000:cl=stereo -pix_fmt yuv420p -c:v libx264 -preset ultrafast -tune zerolatency -bf 0 -g 120 -keyint_min 120 -sc_threshold 0 -c:a aac -b:a 96k -f flv pipe:1
)SH";out.close();chmod(path.c_str(),0755);return path.string();}

opal::VideoKeys client_keys(){opal::VideoKeys k{};for(std::size_t i=0;i<k.send_key.size();++i){k.send_key[i]=static_cast<std::uint8_t>(i+1);k.recv_key[i]=static_cast<std::uint8_t>(0x80+i);}for(std::size_t i=0;i<k.send_nonce_base.size();++i){k.send_nonce_base[i]=static_cast<std::uint8_t>(0x20+i);k.recv_nonce_base[i]=static_cast<std::uint8_t>(0x40+i);}return k;}
opal::VideoKeys server_keys(const opal::VideoKeys&c){opal::VideoKeys s{};s.send_key=c.recv_key;s.recv_key=c.send_key;s.send_nonce_base=c.recv_nonce_base;s.recv_nonce_base=c.send_nonce_base;return s;}
}

int main(){
    auto root=std::filesystem::temp_directory_path()/"opal-direct-video-pipeline";std::filesystem::remove_all(root);std::filesystem::create_directories(root);setenv("OPAL_HOME",root.c_str(),1);setenv("OPAL_CAPTURE_CMD",make_capture_script(root).c_str(),1);setenv("OPAL_AUDIO_TEST_SINK","discard",1);setenv("OPAL_VIDEO_WINDOWED","1",1);
    const auto ck=client_keys(),sk=server_keys(ck);auto sender_socket=opal::open_udp_socket(),receiver_socket=opal::open_udp_socket();assert(sender_socket.fd>=0&&receiver_socket.fd>=0);
    sockaddr_storage sender_peer{},receiver_peer{};socklen_t sender_peer_len=0,receiver_peer_len=0;assert(opal::resolve_udp_endpoint("::1",receiver_socket.local_port,sender_peer,sender_peer_len));assert(opal::resolve_udp_endpoint("::1",sender_socket.local_port,receiver_peer,receiver_peer_len));
    opal::DirectVideoPath sender_path,receiver_path;sender_path.socket=sender_socket;sender_path.peer=sender_peer;sender_path.peer_len=sender_peer_len;sender_path.keys=sk;sender_path.session_id=0x123456789ULL;sender_path.generation=7;receiver_path.socket=receiver_socket;receiver_path.peer=receiver_peer;receiver_path.peer_len=receiver_peer_len;receiver_path.keys=ck;receiver_path.session_id=sender_path.session_id;receiver_path.generation=sender_path.generation;
    std::atomic<bool>sender_ready{false};opal::VideoSender sender;opal::VideoReceiver receiver;auto to_sender=[&](const std::string&line){sender.handle_control_line(line);};auto to_receiver=[&](const std::string&line){receiver.handle_control_line(line);};assert(receiver.start(std::move(receiver_path),to_sender));opal::StreamOptions stream{320,180,60};assert(sender.start(std::move(sender_path),stream,true,to_receiver));sender_ready.store(true);
    const auto deadline=std::chrono::steady_clock::now()+8s;while(!receiver.media_started()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(20ms);assert(receiver.media_started());assert(!receiver.failed());assert(receiver.highest_sequence()>0);
    sender.request_idr();std::this_thread::sleep_for(250ms);assert(sender.target_bitrate()>0);assert(sender.queued_frames()==0);assert(sender.queued_bytes()==0);
    sender.stop();receiver.stop();unsetenv("OPAL_CAPTURE_CMD");unsetenv("OPAL_AUDIO_TEST_SINK");unsetenv("OPAL_VIDEO_WINDOWED");std::filesystem::remove_all(root);return 0;
}
