#include <opal/video_reassembly.hpp>
#include <algorithm>
#include <map>
#include <optional>

namespace opal {
namespace {
constexpr std::size_t kMaxBytes=16u*1024u*1024u;
constexpr std::size_t kMaxFragments=kMaxBytes/kVideoDataFragmentBytes+1;
std::uint16_t read16(const std::uint8_t *p){return static_cast<std::uint16_t>((p[0]<<8)|p[1]);}
}

struct VideoReassembler::Impl {
    struct Frame {
        std::uint64_t id=0,timestamp=0,order=0,video_order=0;
        VideoMediaType type=VideoMediaType::VideoH264;
        bool type_known=false;
        std::uint16_t flags=0,count=0;
        std::vector<std::optional<std::vector<std::uint8_t>>> fragments;
        std::map<std::uint16_t,std::vector<std::uint8_t>> parity;
        std::size_t bytes=0;
    };
    std::uint32_t generation=0;
    std::uint64_t session_id=0,order=0,video_order=0;
    bool active=false,awaiting_idr=false;
    std::map<std::uint64_t,Frame> frames;
    std::size_t bytes=0;
};

VideoReassembler::VideoReassembler():impl_(std::make_unique<Impl>()){}
VideoReassembler::~VideoReassembler()=default;

void VideoReassembler::reset(std::uint32_t generation,std::uint64_t session_id){
    impl_->generation=generation;impl_->session_id=session_id;impl_->active=true;
    impl_->frames.clear();impl_->bytes=0;impl_->order=0;impl_->video_order=0;impl_->awaiting_idr=false;
}

ReassemblyStatus VideoReassembler::accept(const VideoPlainPacket &packet,ReassembledFrame &output){
    return accept(packet.header,packet.payload,output);
}

ReassemblyStatus VideoReassembler::accept(const VideoPacketHeader &header,
                                           std::span<const std::uint8_t> payload,
                                           ReassembledFrame &output){
    auto &impl=*impl_;
    if(!impl.active||header.generation!=impl.generation||header.session_id!=impl.session_id)
        return ReassemblyStatus::Ignored;
    if(payload.size()!=header.payload_length||payload.size()>kVideoPlaintextBytes||
       header.fragment_count==0||header.fragment_count>kMaxFragments)
        return ReassemblyStatus::Ignored;

    bool need_idr=false;
    auto frame_it=impl.frames.find(header.frame_id);
    if(frame_it==impl.frames.end()){
        while(impl.frames.size()>=3){
            auto victim=std::min_element(impl.frames.begin(),impl.frames.end(),[](const auto &a,const auto &b){
                return a.second.order<b.second.order;
            });
            if(victim==impl.frames.end())break;
            if(victim->second.type_known&&victim->second.type==VideoMediaType::VideoH264){
                need_idr=true;impl.awaiting_idr=true;
            }
            impl.bytes-=victim->second.bytes;
            impl.frames.erase(victim);
        }
        Impl::Frame frame;frame.id=header.frame_id;frame.timestamp=header.capture_timestamp_us;
        frame.order=++impl.order;frame.count=header.fragment_count;frame.fragments.resize(frame.count);
        frame_it=impl.frames.emplace(frame.id,std::move(frame)).first;
    }
    auto &frame=frame_it->second;
    if(frame.count!=header.fragment_count)return ReassemblyStatus::Ignored;

    if(header.media_type==VideoMediaType::Fec){
        if(payload.size()<kVideoFecMetadataBytes)return ReassemblyStatus::Ignored;
        const std::size_t start=static_cast<std::size_t>(header.fec_group)*10;
        const std::size_t group_count=payload[0];
        if(group_count==0||group_count>10||start>=frame.count||
           group_count!=std::min<std::size_t>(10,frame.count-start))return ReassemblyStatus::Ignored;
        if(!frame.parity.contains(header.fec_group)){
            if(impl.bytes+payload.size()>kMaxBytes)return ReassemblyStatus::Ignored;
            frame.parity[header.fec_group]=std::vector<std::uint8_t>(payload.begin(),payload.end());
            frame.bytes+=payload.size();impl.bytes+=payload.size();
        }
    }else{
        if(header.fragment_index>=frame.count||payload.size()>kVideoDataFragmentBytes)
            return ReassemblyStatus::Ignored;
        frame.type=header.media_type;frame.type_known=true;frame.flags|=header.flags;

        // frame_id is shared with audio/config units, so use a dedicated count
        // of newly observed H.264 access units as the video freshness clock.
        // Once two newer video frames are visible, an older incomplete H.264
        // reference is no longer useful for a latency-first decoder chain.
        if(frame.type==VideoMediaType::VideoH264&&(frame.flags&FrameConfig)==0&&frame.video_order==0){
            frame.video_order=++impl.video_order;
            for(auto it=impl.frames.begin();it!=impl.frames.end();){
                if(it==frame_it){++it;continue;}
                const auto &older=it->second;
                if(older.type_known&&older.type==VideoMediaType::VideoH264&&older.video_order>0&&
                   frame.video_order>=older.video_order+2){
                    impl.bytes-=older.bytes;
                    it=impl.frames.erase(it);
                    need_idr=true;impl.awaiting_idr=true;
                }else ++it;
            }
        }

        // After a reference loss, dependent H.264 is intentionally discarded.
        // Config units and the next keyframe are still accepted so recovery can
        // resume without a playback/jitter backlog.
        if(frame.type==VideoMediaType::VideoH264&&impl.awaiting_idr&&
           (frame.flags&(FrameKeyframe|FrameConfig))==0){
            impl.bytes-=frame.bytes;
            impl.frames.erase(frame_it);
            return ReassemblyStatus::NeedIdr;
        }

        auto &fragment=frame.fragments[header.fragment_index];
        if(!fragment){
            if(impl.bytes+payload.size()>kMaxBytes)return ReassemblyStatus::Ignored;
            fragment=std::vector<std::uint8_t>(payload.begin(),payload.end());
            frame.bytes+=payload.size();impl.bytes+=payload.size();
        }
    }

    for(const auto &[group,parity]:frame.parity){
        const std::size_t start=static_cast<std::size_t>(group)*10;
        const std::size_t count=parity[0];
        std::size_t missing=0,missing_index=0,longest=0;
        for(std::size_t j=0;j<count;++j){
            const std::size_t length=read16(parity.data()+1+j*2);
            if(length>kVideoDataFragmentBytes)return ReassemblyStatus::Ignored;
            longest=std::max(longest,length);
            if(!frame.fragments[start+j]){++missing;missing_index=start+j;}
        }
        if(kVideoFecMetadataBytes+longest>parity.size())return ReassemblyStatus::Ignored;
        if(missing==1){
            const std::size_t expected=read16(parity.data()+1+(missing_index-start)*2);
            if(impl.bytes+expected>kMaxBytes)return ReassemblyStatus::Ignored;
            std::vector<std::uint8_t> recovered(expected);
            for(std::size_t k=0;k<expected;++k){
                std::uint8_t value=parity[kVideoFecMetadataBytes+k];
                for(std::size_t j=0;j<count;++j){
                    const auto index=start+j;
                    if(index==missing_index||!frame.fragments[index]||k>=frame.fragments[index]->size())continue;
                    value^=(*frame.fragments[index])[k];
                }
                recovered[k]=value;
            }
            frame.fragments[missing_index]=std::move(recovered);
            frame.bytes+=expected;impl.bytes+=expected;
        }
    }

    // FEC alone cannot identify whether a one-fragment unit was H.264 or AAC.
    // Never emit a reconstructed unit until at least one real media fragment
    // has established its type and flags.
    const bool complete=frame.type_known&&std::all_of(frame.fragments.begin(),frame.fragments.end(),[](const auto &fragment){
        return fragment.has_value();
    });
    if(!complete)return need_idr?ReassemblyStatus::NeedIdr:ReassemblyStatus::Incomplete;

    if(frame.type==VideoMediaType::VideoH264&&impl.awaiting_idr&&
       (frame.flags&(FrameKeyframe|FrameConfig))==0){
        impl.bytes-=frame.bytes;impl.frames.erase(frame_it);return ReassemblyStatus::NeedIdr;
    }

    output={};output.media_type=frame.type;output.flags=frame.flags;output.frame_id=frame.id;
    output.capture_timestamp_us=frame.timestamp;output.keyframe=(frame.flags&FrameKeyframe)!=0;
    output.config=(frame.flags&FrameConfig)!=0;
    std::size_t total=0;for(const auto &fragment:frame.fragments)total+=fragment->size();output.data.reserve(total);
    for(const auto &fragment:frame.fragments)output.data.insert(output.data.end(),fragment->begin(),fragment->end());
    if(frame.type==VideoMediaType::VideoH264&&output.keyframe)impl.awaiting_idr=false;
    impl.bytes-=frame.bytes;impl.frames.erase(frame_it);
    return ReassemblyStatus::Complete;
}

std::size_t VideoReassembler::frames_in_flight() const{return impl_->frames.size();}
std::size_t VideoReassembler::bytes_in_flight() const{return impl_->bytes;}
}
