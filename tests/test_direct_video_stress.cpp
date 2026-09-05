#include <opal/video_crypto.hpp>
#include <opal/video_packet.hpp>
#include <opal/video_reassembly.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

namespace {

opal::VideoKeys test_keys(){
    opal::VideoKeys keys;
    for(std::size_t i=0;i<keys.send_key.size();++i)keys.send_key[i]=keys.recv_key[i]=static_cast<std::uint8_t>(i*7+3);
    for(std::size_t i=0;i<keys.send_nonce_base.size();++i)keys.send_nonce_base[i]=keys.recv_nonce_base[i]=static_cast<std::uint8_t>(0xa0+i);
    return keys;
}

std::vector<std::uint8_t> payload_for(std::uint32_t generation,int frame,bool keyframe){
    const bool high_profile=(generation&1U)==0;
    const std::size_t base=high_profile?78u*1024u:52u*1024u;
    const std::size_t variance=high_profile?48u*1024u:32u*1024u;
    const std::size_t key_base=high_profile?320u*1024u:220u*1024u;
    const std::size_t size=keyframe?key_base+static_cast<std::size_t>((generation*8191u+frame*4093u)%(96u*1024u)):
        base+static_cast<std::size_t>((generation*3571u+frame*2377u)%variance);
    std::vector<std::uint8_t> data(size);
    for(std::size_t i=0;i<size;++i)data[i]=static_cast<std::uint8_t>((i*13+generation*19+frame*23)&0xff);
    return data;
}

bool deliver(const opal::VideoKeys&keys,opal::ReplayWindow1024&replay,opal::VideoReassembler&reassembler,
             const opal::VideoPlainPacket&packet,opal::ReassembledFrame&complete,opal::ReassemblyStatus&status){
    auto aad=opal::serialize_video_header(packet.header);std::vector<std::uint8_t> sealed,plain;
    assert(opal::seal_video_datagram(keys,packet.header.packet_sequence,aad,packet.payload,sealed));
    assert(aad.size()+sealed.size()<=opal::kVideoMaxDatagramBytes);
    assert(opal::open_video_datagram(keys,packet.header.packet_sequence,aad,sealed,plain));
    assert(plain==packet.payload);
    if(!replay.accept(packet.header.packet_sequence))return false;
    opal::VideoPlainPacket opened{packet.header,std::move(plain)};status=reassembler.accept(opened,complete);
    assert(reassembler.frames_in_flight()<=3);
    assert(reassembler.bytes_in_flight()<=16u*1024u*1024u);
    return true;
}

}

int main(){
    constexpr std::uint64_t base_sequence=1ULL<<32;
    auto keys=test_keys();std::mt19937 rng(0x0bad5eedu);std::uniform_int_distribution<int> loss_percent(0,3),coin(0,99);
    std::uint64_t stale_drops=0,bytes_exercised=0;std::size_t largest_frame=0,largest_packet_set=0;
    bool saw_single_fec=false,saw_two_loss=false,saw_need_idr=false,saw_recovery=false,saw_burst_loss=false;

    for(std::uint32_t generation=1;generation<=8;++generation){
        const std::uint64_t session_id=0x9000000000000000ULL+generation;
        opal::VideoReassembler reassembler;reassembler.reset(generation,session_id);
        opal::ReplayWindow1024 replay;std::uint64_t sequence=base_sequence+static_cast<std::uint64_t>(generation)*1000000;
        bool need_keyframe=false;

        for(int frame=0;frame<180;++frame){
            const bool forced_incomplete=frame>=10&&frame<=13;
            const bool burst_loss=frame==70;
            bool keyframe=(frame%30)==0||need_keyframe;
            auto payload=payload_for(generation,frame,keyframe);
            bytes_exercised+=payload.size();largest_frame=std::max(largest_frame,payload.size());
            auto packets=opal::fragment_media_unit(opal::VideoMediaType::VideoH264,keyframe?opal::FrameKeyframe:0,
                generation,session_id,static_cast<std::uint64_t>(frame+1),static_cast<std::uint64_t>(frame)*16667,
                payload,sequence,true);
            assert(!packets.empty());largest_packet_set=std::max(largest_packet_set,packets.size());
            for(const auto&p:packets)assert(opal::serialize_video_header(p.header).size()+p.payload.size()+opal::kVideoAeadTagBytes<=opal::kVideoMaxDatagramBytes);

            const bool force_single=(frame==2);
            const bool force_double=(frame==10);
            int dropped_data=0;const int random_loss=loss_percent(rng);
            std::vector<opal::VideoPlainPacket> delivery;delivery.reserve(packets.size());
            for(const auto&packet:packets){
                const bool data=packet.header.media_type!=opal::VideoMediaType::Fec;
                bool drop=false;
                if(force_single&&data&&dropped_data<1){drop=true;++dropped_data;}
                else if(forced_incomplete&&data&&dropped_data<2){drop=true;++dropped_data;}
                else if(burst_loss&&data&&packet.header.fragment_index>=20&&packet.header.fragment_index<26){drop=true;++dropped_data;}
                else if(!force_single&&!forced_incomplete&&!burst_loss&&data&&coin(rng)<random_loss)drop=true;
                if(drop){++stale_drops;continue;}
                delivery.push_back(packet);
            }
            if(force_single){assert(dropped_data==1);saw_single_fec=true;}
            if(force_double){assert(dropped_data==2);saw_two_loss=true;}
            if(forced_incomplete)assert(dropped_data==2);
            if(burst_loss){assert(dropped_data==6);saw_burst_loss=true;}

            for(std::size_t base=0;base<delivery.size();base+=8){const auto end=std::min(delivery.size(),base+8);if(end-base>=4){std::swap(delivery[base],delivery[base+3]);std::swap(delivery[base+1],delivery[end-1]);}}

            bool completed=false;opal::ReassembledFrame assembled;
            for(const auto&packet:delivery){
                opal::ReassemblyStatus status=opal::ReassemblyStatus::Incomplete;opal::ReassembledFrame out;
                if(!deliver(keys,replay,reassembler,packet,out,status))continue;
                if(status==opal::ReassemblyStatus::NeedIdr){saw_need_idr=true;need_keyframe=true;}
                if(status==opal::ReassemblyStatus::Complete){assembled=std::move(out);completed=true;}
            }

            if(force_single){assert(completed);assert(assembled.data==payload);}
            if(forced_incomplete||burst_loss){assert(!completed);need_keyframe=true;}
            if(completed&&need_keyframe&&assembled.keyframe){assert(assembled.data==payload);need_keyframe=false;saw_recovery=true;}
        }

        std::uint64_t duplicate_seq=sequence++;
        auto duplicate_payload=payload_for(generation,200,false);
        auto duplicate_packets=opal::fragment_media_unit(opal::VideoMediaType::VideoH264,0,generation,session_id,500,0,
            duplicate_payload,duplicate_seq,false);
        assert(!duplicate_packets.empty());
        opal::ReassembledFrame ignored;opal::ReassemblyStatus status=opal::ReassemblyStatus::Incomplete;
        const auto&dup=duplicate_packets.front();assert(deliver(keys,replay,reassembler,dup,ignored,status));
        opal::ReassembledFrame ignored2;opal::ReassemblyStatus status2=opal::ReassemblyStatus::Incomplete;
        assert(!deliver(keys,replay,reassembler,dup,ignored2,status2));

        reassembler.reset(generation+1000,session_id+1000);replay.reset();
        std::uint64_t old_seq=sequence+5000;
        auto old_packets=opal::fragment_media_unit(opal::VideoMediaType::VideoH264,opal::FrameKeyframe,generation,session_id,999,0,
            duplicate_payload,old_seq,false);
        assert(!old_packets.empty());
        opal::ReassembledFrame old_out;opal::ReassemblyStatus old_status=opal::ReassemblyStatus::Incomplete;
        assert(deliver(keys,replay,reassembler,old_packets.front(),old_out,old_status));
        assert(old_status==opal::ReassemblyStatus::Ignored);
    }

    assert(bytes_exercised>100u*1024u*1024u);
    assert(largest_frame>300u*1024u);
    assert(largest_packet_set>250);
    assert(stale_drops>0);
    assert(saw_single_fec&&saw_two_loss&&saw_need_idr&&saw_recovery&&saw_burst_loss);
    return 0;
}
