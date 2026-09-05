#include <opal/encoded_buffer_pool.hpp>
#include <opal/video_packet.hpp>
#include <opal/video_reassembly.hpp>

#include <cassert>
#include <cstdint>
#include <vector>

static std::vector<opal::VideoPlainPacket> make_frame(std::uint64_t id,std::size_t bytes,std::uint16_t flags=0){
    std::vector<std::uint8_t>data(bytes);
    for(std::size_t i=0;i<data.size();++i)data[i]=static_cast<std::uint8_t>((i+id*13u)&0xffu);
    std::uint64_t sequence=id*1000;
    return opal::fragment_media_unit(opal::VideoMediaType::VideoH264,flags,5,77,id,id*16667,data,sequence,true);
}

static bool feed_complete(opal::VideoReassembler&reassembler,const std::vector<opal::VideoPlainPacket>&packets,opal::ReassembledFrame&complete){
    bool delivered=false;
    for(const auto&packet:packets)if(reassembler.accept(packet,complete)==opal::ReassemblyStatus::Complete)delivered=true;
    return delivered;
}

int main(){
    std::vector<std::uint8_t>expected(100*1024);
    for(std::size_t i=0;i<expected.size();++i)expected[i]=static_cast<std::uint8_t>((i+13u)&0xffu);
    std::uint64_t sequence=1000;
    auto packets=opal::fragment_media_unit(opal::VideoMediaType::VideoH264,opal::FrameKeyframe,5,77,1,16667,expected,sequence,true);
    opal::VideoReassembler reassembler;
    reassembler.reset(5,77);
    opal::ReassembledFrame complete;
    bool completed=false;
    for(const auto&packet:packets){
        if(packet.header.media_type!=opal::VideoMediaType::Fec&&packet.header.fragment_index==3)continue;
        if(reassembler.accept(packet,complete)==opal::ReassemblyStatus::Complete){completed=true;break;}
    }
    assert(completed&&complete.data==expected&&complete.frame_id==1&&complete.keyframe);

    reassembler.reset(5,77);
    completed=false;
    for(const auto&packet:packets){
        if(packet.header.media_type!=opal::VideoMediaType::Fec&&(packet.header.fragment_index==2||packet.header.fragment_index==3))continue;
        if(reassembler.accept(packet,complete)==opal::ReassemblyStatus::Complete)completed=true;
    }
    assert(!completed);

    std::vector<std::uint8_t>tiny_expected(500);
    for(std::size_t i=0;i<tiny_expected.size();++i)tiny_expected[i]=static_cast<std::uint8_t>((i+91u)&0xffu);
    sequence=60000;
    auto tiny_packets=opal::fragment_media_unit(opal::VideoMediaType::VideoH264,opal::FrameKeyframe,5,77,60,1000020,tiny_expected,sequence,true);
    assert(tiny_packets.size()==2);
    reassembler.reset(5,77);
    completed=false;
    for(const auto&packet:tiny_packets){
        if(packet.header.media_type!=opal::VideoMediaType::Fec)continue;
        if(reassembler.accept(packet,complete)==opal::ReassemblyStatus::Complete){completed=true;break;}
    }
    assert(completed&&complete.data==tiny_expected&&complete.frame_id==60&&complete.keyframe);

    reassembler.reset(5,77);
    auto startup_p=make_frame(40,500);
    bool startup_complete=false,startup_need_idr=false;
    for(const auto&packet:startup_p){
        const auto status=reassembler.accept(packet,complete);
        startup_complete|=status==opal::ReassemblyStatus::Complete;
        startup_need_idr|=status==opal::ReassemblyStatus::NeedIdr;
    }
    assert(!startup_complete&&startup_need_idr);

    // One delayed reference frame is tolerated inside the fixed reorder window.
    reassembler.reset(5,77);
    auto initial=make_frame(49,500,opal::FrameKeyframe);
    assert(feed_complete(reassembler,initial,complete));
    auto delayed_reference=make_frame(50,opal::kVideoDataFragmentBytes+100);
    assert(reassembler.accept(delayed_reference.front(),complete)==opal::ReassemblyStatus::Incomplete);
    auto next_reference=make_frame(51,500);
    bool next_complete=false,false_idr=false;
    for(const auto&packet:next_reference){
        const auto status=reassembler.accept(packet,complete);
        next_complete|=status==opal::ReassemblyStatus::Complete;
        false_idr|=status==opal::ReassemblyStatus::NeedIdr;
    }
    assert(!next_complete&&!false_idr);
    bool delayed_complete=false;
    for(std::size_t i=1;i<delayed_reference.size();++i){
        const auto status=reassembler.accept(delayed_reference[i],complete);
        if(status==opal::ReassemblyStatus::Complete){delayed_complete=true;assert(complete.frame_id==50);}
        assert(status!=opal::ReassemblyStatus::NeedIdr);
    }
    assert(delayed_complete);
    assert(reassembler.drain(complete)==opal::ReassemblyStatus::Complete&&complete.frame_id==51);
    bool late_old_complete=false;
    for(const auto&packet:delayed_reference)if(reassembler.accept(packet,complete)==opal::ReassemblyStatus::Complete)late_old_complete=true;
    assert(!late_old_complete);

    // A completely absent frame is tolerated only until the bounded three-frame
    // reorder window is exhausted; then the chain really is unrecoverable.
    reassembler.reset(5,77);
    auto gap_base=make_frame(100,500,opal::FrameKeyframe);
    assert(feed_complete(reassembler,gap_base,complete));
    for(std::uint64_t id=102;id<=104;++id){
        auto future=make_frame(id,500);
        bool gap_need_idr=false,gap_complete=false;
        for(const auto&packet:future){
            const auto status=reassembler.accept(packet,complete);
            gap_need_idr|=status==opal::ReassemblyStatus::NeedIdr;
            gap_complete|=status==opal::ReassemblyStatus::Complete;
        }
        assert(!gap_need_idr&&!gap_complete);
    }
    auto overflow=make_frame(105,500);
    bool overflow_need_idr=false;
    for(const auto&packet:overflow)overflow_need_idr|=reassembler.accept(packet,complete)==opal::ReassemblyStatus::NeedIdr;
    assert(overflow_need_idr);
    auto gap_key=make_frame(106,500,opal::FrameKeyframe);
    assert(feed_complete(reassembler,gap_key,complete));

    reassembler.reset(5,77);
    auto bound_key=make_frame(9,500,opal::FrameKeyframe);
    assert(feed_complete(reassembler,bound_key,complete));
    opal::ReassemblyStatus fourth=opal::ReassemblyStatus::Incomplete;
    for(std::uint64_t id=10;id<14;++id){
        auto frame=make_frame(id,opal::kVideoDataFragmentBytes+100);
        for(const auto&packet:frame){
            if(packet.header.media_type!=opal::VideoMediaType::Fec&&packet.header.fragment_index==0){fourth=reassembler.accept(packet,complete);break;}
        }
    }
    assert(reassembler.frames_in_flight()<=3);
    assert(reassembler.bytes_in_flight()<=16u*1024u*1024u);
    assert(fourth==opal::ReassemblyStatus::NeedIdr);

    reassembler.reset(5,77);
    opal::VideoPlainPacket oversized;
    oversized.header.generation=5;oversized.header.session_id=77;oversized.header.frame_id=20;oversized.header.fragment_count=1;
    oversized.header.payload_length=static_cast<std::uint16_t>(opal::kVideoPlaintextBytes);
    oversized.payload.resize(opal::kVideoPlaintextBytes+1);
    assert(reassembler.accept(oversized,complete)==opal::ReassemblyStatus::Ignored);
    assert(reassembler.bytes_in_flight()==0);

    reassembler.reset(5,77);
    auto reset_key=make_frame(29,500,opal::FrameKeyframe);
    assert(feed_complete(reassembler,reset_key,complete));
    auto old=make_frame(30,opal::kVideoDataFragmentBytes+10);
    for(const auto&packet:old){
        if(packet.header.media_type!=opal::VideoMediaType::Fec){reassembler.accept(packet,complete);break;}
    }
    assert(reassembler.frames_in_flight()==1);
    reassembler.reset(6,88);
    assert(reassembler.frames_in_flight()==0&&reassembler.bytes_in_flight()==0);
    assert(reassembler.accept(old.front(),complete)==opal::ReassemblyStatus::Ignored);

    reassembler.reset(5,77);
    for(std::uint64_t cycle=1;cycle<=128;++cycle){
        auto frame=make_frame(1000+cycle,700,opal::FrameKeyframe);
        assert(feed_complete(reassembler,frame,complete));
        assert(reassembler.frames_in_flight()==0);
        assert(reassembler.bytes_in_flight()==0);
    }

    reassembler.reset(5,77);
    std::vector<std::uint8_t>aligned_expected(opal::kVideoDataFragmentBytes*3);
    for(std::size_t i=0;i<aligned_expected.size();++i)aligned_expected[i]=static_cast<std::uint8_t>((i*7u)&0xffu);
    sequence=900000;
    auto aligned=opal::fragment_media_unit(opal::VideoMediaType::VideoH264,opal::FrameKeyframe,5,77,900,15000000,aligned_expected,sequence,false);
    assert(aligned.size()==3);
    assert(reassembler.accept(aligned[0],complete)==opal::ReassemblyStatus::Incomplete);
    assert(reassembler.accept(aligned[1],complete)==opal::ReassemblyStatus::Incomplete);
    assert(reassembler.accept(aligned[2],complete)==opal::ReassemblyStatus::Complete);
    assert(complete.data==aligned_expected);
    assert(complete.data.capacity()>=complete.data.size()+64);

    reassembler.reset(5,77);
    auto malformed_fec=make_frame(950,opal::kVideoDataFragmentBytes+100,opal::FrameKeyframe);
    opal::VideoPlainPacket malformed_parity,final_data;
    for(const auto&packet:malformed_fec){
        if(packet.header.media_type==opal::VideoMediaType::Fec)malformed_parity=packet;
        else if(packet.header.fragment_index==1)final_data=packet;
    }
    assert(!malformed_parity.payload.empty()&&!final_data.payload.empty());
    malformed_parity.payload[1]=0;malformed_parity.payload[2]=100;
    malformed_parity.header.payload_length=static_cast<std::uint16_t>(malformed_parity.payload.size());
    assert(reassembler.accept(final_data,complete)==opal::ReassemblyStatus::Incomplete);
    assert(reassembler.accept(malformed_parity,complete)==opal::ReassemblyStatus::Ignored);

    auto&pool=opal::encoded_buffer_pool();
    auto pooled=pool.acquire(300000,300000);auto*pooled_ptr=pooled.data();pool.release(std::move(pooled));
    auto reused=pool.acquire(200000,200000);assert(reused.data()==pooled_ptr);pool.release(std::move(reused));
    assert(pool.cached_buffers()>=1&&pool.cached_buffers()<=opal::EncodedBufferPool::kSlots);
    assert(pool.cached_bytes()<=opal::EncodedBufferPool::kMaxRetainedBytes);
    return 0;
}
