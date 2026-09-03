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
        std::uint64_t id=0,timestamp=0,order=0;
        VideoMediaType type=VideoMediaType::VideoH264;
        bool type_known=false;
        std::uint16_t flags=0,count=0;
        std::vector<std::optional<std::vector<std::uint8_t>>> fragments;
        std::map<std::uint16_t,std::vector<std::uint8_t>> parity;
        std::size_t bytes=0;
    };
    std::uint32_t generation=0;
    std::uint64_t session_id=0,order=0;
    bool active=false;
    std::map<std::uint64_t,Frame> frames;
    std::size_t bytes=0;
};

VideoReassembler::VideoReassembler():impl_(std::make_unique<Impl>()){}
VideoReassembler::~VideoReassembler()=default;

void VideoReassembler::reset(std::uint32_t generation,std::uint64_t session_id){
    impl_->generation=generation;impl_->session_id=session_id;impl_->active=true;
    impl_->frames.clear();impl_->bytes=0;impl_->order=0;
}

ReassemblyStatus VideoReassembler::accept(const VideoPlainPacket &packet,ReassembledFrame &output){
    auto &impl=*impl_;
    if(!impl.active||packet.header.generation!=impl.generation||packet.header.session_id!=impl.session_id)
        return ReassemblyStatus::Ignored;
    if(packet.payload.size()!=packet.header.payload_length||packet.payload.size()>kVideoPlaintextBytes||
       packet.header.fragment_count==0||packet.header.fragment_count>kMaxFragments)
        return ReassemblyStatus::Ignored;

    bool need_idr=false;
    auto frame_it=impl.frames.find(packet.header.frame_id);
    if(frame_it==impl.frames.end()){
        while(impl.frames.size()>=3){
            auto victim=std::min_element(impl.frames.begin(),impl.frames.end(),[](const auto &a,const auto &b){
                return a.second.order<b.second.order;
            });
            if(victim==impl.frames.end())break;
            if(victim->second.type_known&&victim->second.type==VideoMediaType::VideoH264)need_idr=true;
            impl.bytes-=victim->second.bytes;
            impl.frames.erase(victim);
        }
        Impl::Frame frame;frame.id=packet.header.frame_id;frame.timestamp=packet.header.capture_timestamp_us;
        frame.order=++impl.order;frame.count=packet.header.fragment_count;frame.fragments.resize(frame.count);
        frame_it=impl.frames.emplace(frame.id,std::move(frame)).first;
    }
    auto &frame=frame_it->second;
    if(frame.count!=packet.header.fragment_count)return ReassemblyStatus::Ignored;

    if(packet.header.media_type==VideoMediaType::Fec){
        if(packet.payload.size()<kVideoFecMetadataBytes)return ReassemblyStatus::Ignored;
        const std::size_t start=static_cast<std::size_t>(packet.header.fec_group)*10;
        const std::size_t group_count=packet.payload[0];
        if(group_count==0||group_count>10||start>=frame.count||
           group_count!=std::min<std::size_t>(10,frame.count-start))return ReassemblyStatus::Ignored;
        if(!frame.parity.contains(packet.header.fec_group)){
            if(impl.bytes+packet.payload.size()>kMaxBytes)return ReassemblyStatus::Ignored;
            frame.parity[packet.header.fec_group]=packet.payload;
            frame.bytes+=packet.payload.size();impl.bytes+=packet.payload.size();
        }
    }else{
        if(packet.header.fragment_index>=frame.count||packet.payload.size()>kVideoDataFragmentBytes)
            return ReassemblyStatus::Ignored;
        frame.type=packet.header.media_type;frame.type_known=true;frame.flags|=packet.header.flags;
        auto &fragment=frame.fragments[packet.header.fragment_index];
        if(!fragment){
            if(impl.bytes+packet.payload.size()>kMaxBytes)return ReassemblyStatus::Ignored;
            fragment=packet.payload;frame.bytes+=packet.payload.size();impl.bytes+=packet.payload.size();
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

    const bool complete=std::all_of(frame.fragments.begin(),frame.fragments.end(),[](const auto &fragment){
        return fragment.has_value();
    });
    if(!complete)return need_idr?ReassemblyStatus::NeedIdr:ReassemblyStatus::Incomplete;

    output={};output.media_type=frame.type;output.flags=frame.flags;output.frame_id=frame.id;
    output.capture_timestamp_us=frame.timestamp;output.keyframe=(frame.flags&FrameKeyframe)!=0;
    output.config=(frame.flags&FrameConfig)!=0;
    std::size_t total=0;for(const auto &fragment:frame.fragments)total+=fragment->size();output.data.reserve(total);
    for(const auto &fragment:frame.fragments)output.data.insert(output.data.end(),fragment->begin(),fragment->end());
    impl.bytes-=frame.bytes;impl.frames.erase(frame_it);
    return ReassemblyStatus::Complete;
}

std::size_t VideoReassembler::frames_in_flight() const{return impl_->frames.size();}
std::size_t VideoReassembler::bytes_in_flight() const{return impl_->bytes;}
}
