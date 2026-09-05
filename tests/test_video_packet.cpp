#include <opal/video_packet.hpp>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <set>
#include <thread>
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

    assert(opal::use_fec_for_media(opal::VideoMediaType::VideoH264));
    assert(!opal::use_fec_for_media(opal::VideoMediaType::AudioAac));
    assert(!opal::use_fec_for_media(opal::VideoMediaType::Probe));
    assert(!opal::use_fec_for_media(opal::VideoMediaType::Keepalive));

    std::uint64_t keepalive_sequence=4000;
    opal::VideoFragmentCursor keepalive(opal::VideoMediaType::Keepalive,0,7,99,100,1234,{},keepalive_sequence,false);
    assert(keepalive.valid());opal::VideoPacketHeader keepalive_header;std::span<const std::uint8_t> keepalive_payload;
    assert(keepalive.next(keepalive_header,keepalive_payload));assert(keepalive_header.media_type==opal::VideoMediaType::Keepalive);
    assert(keepalive_header.payload_length==0&&keepalive_payload.empty());assert(!keepalive.next(keepalive_header,keepalive_payload));
    auto keepalive_wire=opal::serialize_video_header(keepalive_header);opal::VideoPacketHeader keepalive_parsed;
    assert(opal::parse_video_header(keepalive_wire,keepalive_parsed));assert(keepalive_parsed.media_type==opal::VideoMediaType::Keepalive);

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

    std::uint64_t cursor_sequence=5000;
    opal::VideoFragmentCursor cursor(opal::VideoMediaType::VideoH264,opal::FrameKeyframe,
        3,99,11,888000,frame,cursor_sequence,true);
    assert(cursor.valid());
    std::size_t cursor_data=0,cursor_fec=0;opal::VideoPacketHeader cursor_header;std::span<const std::uint8_t> cursor_payload;
    while(cursor.next(cursor_header,cursor_payload)){
        assert(opal::kVideoHeaderBytes+cursor_payload.size()+opal::kVideoAeadTagBytes<=opal::kVideoMaxDatagramBytes);
        if(cursor_header.media_type==opal::VideoMediaType::Fec){++cursor_fec;assert(cursor_payload.size()>=opal::kVideoFecMetadataBytes);}
        else{++cursor_data;const auto offset=static_cast<std::size_t>(cursor_header.fragment_index)*opal::kVideoDataFragmentBytes;assert(cursor_payload.data()==frame.data()+offset);}
    }
    assert(cursor_data==data_packets&&cursor_fec==fec_packets);
    assert(cursor_sequence==5000+cursor_data+cursor_fec);

    std::atomic<std::uint64_t> concurrent_sequence{9000};
    std::vector<std::uint64_t> sequence_a,sequence_b;
    auto fragment_job=[&](std::uint64_t frame_id,std::vector<std::uint64_t>&out){
        std::vector<std::uint8_t> small(8*opal::kVideoDataFragmentBytes,0x5a);
        opal::VideoFragmentCursor c(opal::VideoMediaType::VideoH264,0,3,99,frame_id,999000,small,concurrent_sequence,false);
        opal::VideoPacketHeader h;std::span<const std::uint8_t> p;
        while(c.next(h,p))out.push_back(h.packet_sequence);
    };
    std::thread ta(fragment_job,21,std::ref(sequence_a));
    std::thread tb(fragment_job,22,std::ref(sequence_b));
    ta.join();tb.join();
    std::set<std::uint64_t> unique;unique.insert(sequence_a.begin(),sequence_a.end());unique.insert(sequence_b.begin(),sequence_b.end());
    assert(sequence_a.size()==8&&sequence_b.size()==8&&unique.size()==16);
    assert(*unique.begin()==9000&&*unique.rbegin()==9015&&concurrent_sequence.load()==9016);
    return 0;
}
