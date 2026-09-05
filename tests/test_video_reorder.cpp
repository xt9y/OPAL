#include <opal/video_packet.hpp>
#include <opal/video_reassembly.hpp>

#include <cassert>
#include <cstdint>
#include <vector>

namespace {

std::vector<opal::VideoPlainPacket> frame(std::uint64_t id,std::size_t bytes,std::uint16_t flags=0){
    std::vector<std::uint8_t> data(bytes);
    for(std::size_t i=0;i<data.size();++i)data[i]=static_cast<std::uint8_t>((i+id*13u)&0xffu);
    std::uint64_t sequence=id*1000;
    return opal::fragment_media_unit(opal::VideoMediaType::VideoH264,flags,5,77,id,id*16667,data,sequence,false);
}

bool feed_complete(opal::VideoReassembler&reassembler,const std::vector<opal::VideoPlainPacket>&packets,opal::ReassembledFrame&out){
    bool complete=false;
    for(const auto&packet:packets)if(reassembler.accept(packet,out)==opal::ReassemblyStatus::Complete)complete=true;
    return complete;
}

}

int main(){
    {
        opal::VideoReassembler reassembler;
        reassembler.reset(5,77);
        opal::ReassembledFrame out;

        const auto key=frame(200,500,opal::FrameKeyframe);
        assert(feed_complete(reassembler,key,out));
        assert(out.frame_id==200&&out.keyframe);

        const auto delayed=frame(201,opal::kVideoDataFragmentBytes+100);
        const auto future=frame(202,500);
        assert(delayed.size()==2);
        assert(reassembler.accept(delayed.front(),out)==opal::ReassemblyStatus::Incomplete);

        bool future_complete=false;
        bool false_idr=false;
        for(const auto&packet:future){
            const auto status=reassembler.accept(packet,out);
            future_complete|=status==opal::ReassemblyStatus::Complete;
            false_idr|=status==opal::ReassemblyStatus::NeedIdr;
        }

        // A later frame completing first is packet reordering, not proof of loss.
        // Keep it inside the bounded three-frame reassembly window until 201 arrives.
        assert(!false_idr);
        assert(!future_complete);
        assert(reassembler.frames_in_flight()==2);

        assert(reassembler.accept(delayed.back(),out)==opal::ReassemblyStatus::Complete);
        assert(out.frame_id==201&&!out.keyframe);
        assert(reassembler.drain(out)==opal::ReassemblyStatus::Complete);
        assert(out.frame_id==202&&!out.keyframe);
        assert(reassembler.drain(out)==opal::ReassemblyStatus::Incomplete);
        assert(reassembler.frames_in_flight()==0);
    }

    {
        // Startup jitter may deliver a P-frame before the final fragment of the
        // keyframe that will satisfy awaiting_idr. That is not evidence that the
        // keyframe was lost: keep the dependent frame in the existing bounded
        // reorder window until the keyframe finishes.
        opal::VideoReassembler reassembler;
        reassembler.reset(5,77);
        opal::ReassembledFrame out;

        const auto config=frame(1,64,opal::FrameConfig);
        assert(feed_complete(reassembler,config,out));
        assert(out.frame_id==1&&out.config);

        const auto key=frame(2,opal::kVideoDataFragmentBytes+100,opal::FrameKeyframe);
        const auto dependent=frame(3,500);
        assert(key.size()==2);
        assert(reassembler.accept(key.front(),out)==opal::ReassemblyStatus::Incomplete);

        bool false_idr=false;
        for(const auto&packet:dependent)false_idr|=reassembler.accept(packet,out)==opal::ReassemblyStatus::NeedIdr;
        assert(!false_idr);
        assert(reassembler.frames_in_flight()==2);

        assert(reassembler.accept(key.back(),out)==opal::ReassemblyStatus::Complete);
        assert(out.frame_id==2&&out.keyframe);
        assert(reassembler.drain(out)==opal::ReassemblyStatus::Complete);
        assert(out.frame_id==3&&!out.keyframe);
        assert(reassembler.frames_in_flight()==0);
    }
    return 0;
}
