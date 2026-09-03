#include <opal/video_packet.hpp>
#include <cassert>
#include <cstdint>
#include <vector>

int main(){
    opal::VideoPacketHeader header;
    header.flags=opal::FrameKeyframe|opal::FrameEnd;
    header.generation=7;header.session_id=0x1122334455667788ULL;header.packet_sequence=91;
    header.frame_id=42;header.capture_timestamp_us=123456789;header.fragment_index=2;
    header.fragment_count=9;header.fec_group=3;header.payload_length=5;
    opal::VideoPlainPacket packet{header,{1,2,3,4,5}};
    auto wire=opal::serialize_plain_video_packet(packet);
    assert(wire.size()==opal::kVideoHeaderBytes+5);
    opal::VideoPlainPacket parsed;
    assert(opal::parse_plain_video_packet(wire,parsed));
    assert(parsed.header.generation==7&&parsed.header.session_id==header.session_id);
    assert(parsed.header.packet_sequence==91&&parsed.header.frame_id==42);
    assert(parsed.header.fragment_index==2&&parsed.header.fragment_count==9);
    assert(parsed.payload==packet.payload);
    auto bad_magic=wire;bad_magic[0]^=1;assert(!opal::parse_plain_video_packet(bad_magic,parsed));
    auto bad_version=wire;bad_version[4]=2;assert(!opal::parse_plain_video_packet(bad_version,parsed));
    auto bad_length=wire;bad_length.pop_back();assert(!opal::parse_plain_video_packet(bad_length,parsed));

    std::vector<std::uint8_t> frame(100*1024);
    for(std::size_t i=0;i<frame.size();++i)frame[i]=static_cast<std::uint8_t>((i*17u)&0xffu);
    std::uint64_t sequence=1;
    auto fragments=opal::fragment_media_unit(opal::VideoMediaType::VideoH264,opal::FrameKeyframe,
        3,99,10,777000,frame,sequence,true);
    std::size_t data_packets=0,fec_packets=0;
    for(const auto &fragment:fragments){
        assert(opal::kVideoHeaderBytes+fragment.payload.size()+opal::kVideoAeadTagBytes<=opal::kVideoMaxDatagramBytes);
        if(fragment.header.media_type==opal::VideoMediaType::Fec){
            ++fec_packets;assert(!fragment.payload.empty());assert(fragment.payload[0]>=1&&fragment.payload[0]<=10);
        }else{
            ++data_packets;assert(fragment.payload.size()<=opal::kVideoDataFragmentBytes);
        }
    }
    assert(data_packets==(frame.size()+opal::kVideoDataFragmentBytes-1)/opal::kVideoDataFragmentBytes);
    assert(fec_packets==(data_packets+9)/10);
    assert(sequence==1+fragments.size());
    return 0;
}
