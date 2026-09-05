#include <opal/video_reassembly.hpp>
#include <opal/encoded_buffer_pool.hpp>

#include <algorithm>
#include <array>
#include <cstring>

namespace opal {
namespace {
constexpr std::size_t kMaxBytes=16u*1024u*1024u;
constexpr std::size_t kMaxFragments=kMaxBytes/kVideoDataFragmentBytes+1;
constexpr std::size_t kFrameSlots=3;
constexpr std::size_t kDecoderPaddingReserve=64;

std::uint16_t read16(const std::uint8_t*p){return static_cast<std::uint16_t>((p[0]<<8)|p[1]);}

std::size_t storage_for(std::size_t count){
    if(count==0||count>kMaxFragments)return 0;
    const std::size_t groups=(count+9)/10;
    const std::size_t data=count*kVideoDataFragmentBytes+kDecoderPaddingReserve;
    const std::size_t meta=count*(sizeof(std::uint16_t)+sizeof(std::uint8_t));
    const std::size_t fec=groups*kVideoPlaintextBytes+groups*(sizeof(std::uint16_t)+sizeof(std::uint8_t));
    if(data>kMaxBytes||meta>kMaxBytes-data||fec>kMaxBytes-data-meta)return 0;
    return data+meta+fec;
}
}

struct VideoReassembler::Impl{
    struct Frame{
        bool active=false,complete=false;
        std::uint64_t id=0,timestamp=0,order=0;
        VideoMediaType type=VideoMediaType::VideoH264;
        bool type_known=false;
        std::uint16_t flags=0,count=0;
        std::vector<std::uint8_t> fragment_data,fragment_present,parity_data,parity_present;
        std::vector<std::uint16_t> fragment_lengths,parity_lengths;
        std::size_t storage_bytes=0;

        bool init(std::uint64_t frame_id,std::uint64_t capture_timestamp,std::uint64_t insertion_order,std::uint16_t fragments){
            const auto planned=storage_for(fragments);
            if(!planned)return false;
            active=true;complete=false;id=frame_id;timestamp=capture_timestamp;order=insertion_order;
            type=VideoMediaType::VideoH264;type_known=false;flags=0;count=fragments;storage_bytes=planned;
            const std::size_t group_count=(count+9)/10;
            const std::size_t data_bytes=static_cast<std::size_t>(count)*kVideoDataFragmentBytes;
            const std::size_t required_capacity=data_bytes+kDecoderPaddingReserve;
            if(fragment_data.capacity()<required_capacity){
                if(fragment_data.capacity()<=EncodedBufferPool::kMaxRetainedBufferBytes)encoded_buffer_pool().release(std::move(fragment_data));
                else fragment_data={};
                fragment_data=encoded_buffer_pool().acquire(required_capacity,data_bytes);
            }else fragment_data.resize(data_bytes);
            fragment_present.resize(count);std::fill(fragment_present.begin(),fragment_present.end(),0);
            fragment_lengths.resize(count);std::fill(fragment_lengths.begin(),fragment_lengths.end(),0);
            parity_data.resize(group_count*kVideoPlaintextBytes);
            parity_present.resize(group_count);std::fill(parity_present.begin(),parity_present.end(),0);
            parity_lengths.resize(group_count);std::fill(parity_lengths.begin(),parity_lengths.end(),0);
            return true;
        }

        void deactivate(){
            active=false;complete=false;id=timestamp=order=0;type=VideoMediaType::VideoH264;type_known=false;flags=0;count=0;storage_bytes=0;
            if(fragment_data.capacity()>EncodedBufferPool::kMaxRetainedBufferBytes)fragment_data={};
            fragment_present.clear();fragment_lengths.clear();parity_data.clear();parity_present.clear();parity_lengths.clear();
        }

        std::size_t groups()const{return count?(static_cast<std::size_t>(count)+9)/10:0;}
        std::uint8_t*fragment_ptr(std::size_t index){return fragment_data.data()+index*kVideoDataFragmentBytes;}
        const std::uint8_t*fragment_ptr(std::size_t index)const{return fragment_data.data()+index*kVideoDataFragmentBytes;}
        std::uint8_t*parity_ptr(std::size_t group){return parity_data.data()+group*kVideoPlaintextBytes;}
        const std::uint8_t*parity_ptr(std::size_t group)const{return parity_data.data()+group*kVideoPlaintextBytes;}
    };

    std::uint32_t generation=0;
    std::uint64_t session_id=0,order=0,last_video_id=0;
    bool active=false,awaiting_idr=false;
    std::array<Frame,kFrameSlots>frames{};
    std::size_t bytes=0;

    Frame*find(std::uint64_t id){for(auto&frame:frames)if(frame.active&&frame.id==id)return &frame;return nullptr;}
    const Frame*find(std::uint64_t id)const{for(const auto&frame:frames)if(frame.active&&frame.id==id)return &frame;return nullptr;}
    std::size_t count()const{std::size_t result=0;for(const auto&frame:frames)if(frame.active)++result;return result;}
    Frame*free_slot(){for(auto&frame:frames)if(!frame.active)return &frame;return nullptr;}
    Frame*oldest(){Frame*victim=nullptr;for(auto&frame:frames)if(frame.active&&(!victim||frame.order<victim->order))victim=&frame;return victim;}
    bool has_inflight_keyframe_before(std::uint64_t id)const{
        for(const auto&frame:frames){
            if(!frame.active||frame.id>=id||!frame.type_known||frame.type!=VideoMediaType::VideoH264)continue;
            if((frame.flags&FrameKeyframe)!=0)return true;
        }
        return false;
    }

    void release(Frame&frame){
        if(!frame.active)return;
        if(bytes>=frame.storage_bytes)bytes-=frame.storage_bytes;else bytes=0;
        frame.deactivate();
    }

    void clear_active(){for(auto&frame:frames)if(frame.active)frame.deactivate();bytes=0;}

    void release_older_video(std::uint64_t id){
        for(auto&frame:frames)if(frame.active&&frame.id<id&&frame.type_known&&frame.type==VideoMediaType::VideoH264)release(frame);
    }

    void emit(Frame&frame,ReassembledFrame&output){
        output.media_type=frame.type;
        output.flags=frame.flags;
        output.frame_id=frame.id;
        output.capture_timestamp_us=frame.timestamp;
        output.keyframe=(frame.flags&FrameKeyframe)!=0;
        output.config=(frame.flags&FrameConfig)!=0;
        const std::size_t total=frame.count?((static_cast<std::size_t>(frame.count)-1)*kVideoDataFragmentBytes+frame.fragment_lengths.back()):0;
        output.data=std::move(frame.fragment_data);
        output.data.resize(total);
        if(frame.type==VideoMediaType::VideoH264){
            last_video_id=std::max(last_video_id,frame.id);
            if(output.keyframe)awaiting_idr=false;
        }
        release(frame);
    }

    Frame*next_ready_video(){
        if(awaiting_idr||last_video_id==0)return nullptr;
        auto*frame=find(last_video_id+1);
        if(!frame||!frame->complete||!frame->type_known||frame->type!=VideoMediaType::VideoH264)return nullptr;
        return frame;
    }

    ReassemblyStatus emit_ready_or(ReassembledFrame&output,ReassemblyStatus fallback){
        auto*ready=next_ready_video();
        if(!ready)return fallback;
        emit(*ready,output);
        return ReassemblyStatus::Complete;
    }
};

VideoReassembler::VideoReassembler():impl_(std::make_unique<Impl>()){}
VideoReassembler::~VideoReassembler()=default;

void VideoReassembler::reset(std::uint32_t generation,std::uint64_t session_id){
    impl_->generation=generation;impl_->session_id=session_id;impl_->active=true;impl_->clear_active();impl_->order=0;impl_->last_video_id=0;impl_->awaiting_idr=true;
}

void VideoReassembler::require_idr(){
    if(!impl_)return;
    impl_->awaiting_idr=true;
    for(auto&frame:impl_->frames)if(frame.active&&frame.type_known&&frame.type==VideoMediaType::VideoH264)impl_->release(frame);
}

ReassemblyStatus VideoReassembler::accept(const VideoPlainPacket&packet,ReassembledFrame&output){return accept(packet.header,packet.payload,output);}

ReassemblyStatus VideoReassembler::accept(const VideoPacketHeader&header,std::span<const std::uint8_t>payload,ReassembledFrame&output){
    auto&impl=*impl_;
    if(!impl.active||header.generation!=impl.generation||header.session_id!=impl.session_id)return ReassemblyStatus::Ignored;
    if(payload.size()!=header.payload_length||payload.size()>kVideoPlaintextBytes||header.fragment_count==0||header.fragment_count>kMaxFragments)return ReassemblyStatus::Ignored;
    const bool video_packet=header.media_type==VideoMediaType::VideoH264||header.media_type==VideoMediaType::Fec;
    if(video_packet&&impl.last_video_id&&header.frame_id<=impl.last_video_id)return ReassemblyStatus::Ignored;

    bool need_idr=false;
    auto*frame=impl.find(header.frame_id);
    if(!frame){
        const std::size_t planned=storage_for(header.fragment_count);
        if(!planned)return ReassemblyStatus::Ignored;
        while(impl.count()>=kFrameSlots||impl.bytes+planned>kMaxBytes){
            auto*victim=impl.oldest();
            if(!victim)break;
            if(victim->type_known&&victim->type==VideoMediaType::VideoH264){need_idr=true;impl.awaiting_idr=true;}
            impl.release(*victim);
        }
        if(impl.bytes+planned>kMaxBytes)return need_idr?ReassemblyStatus::NeedIdr:ReassemblyStatus::Ignored;
        frame=impl.free_slot();
        if(!frame)return need_idr?ReassemblyStatus::NeedIdr:ReassemblyStatus::Ignored;
        if(!frame->init(header.frame_id,header.capture_timestamp_us,++impl.order,header.fragment_count))return ReassemblyStatus::Ignored;
        impl.bytes+=frame->storage_bytes;
    }
    if(frame->count!=header.fragment_count)return ReassemblyStatus::Ignored;

    if(header.media_type==VideoMediaType::Fec){
        if(payload.size()<kVideoFecMetadataBytes||header.fec_group>=frame->groups())return ReassemblyStatus::Ignored;
        const std::size_t start=static_cast<std::size_t>(header.fec_group)*10;
        const std::size_t group_count=payload[0];
        if(group_count==0||group_count>10||start>=frame->count||group_count!=std::min<std::size_t>(10,frame->count-start))return ReassemblyStatus::Ignored;
        if(frame->type_known&&frame->type!=VideoMediaType::VideoH264)return ReassemblyStatus::Ignored;
        frame->type=VideoMediaType::VideoH264;frame->type_known=true;frame->flags|=header.flags;
        if(!frame->parity_present[header.fec_group]){
            std::memcpy(frame->parity_ptr(header.fec_group),payload.data(),payload.size());
            frame->parity_lengths[header.fec_group]=static_cast<std::uint16_t>(payload.size());
            frame->parity_present[header.fec_group]=1;
        }
    }else{
        if(header.fragment_index>=frame->count||payload.size()>kVideoDataFragmentBytes)return ReassemblyStatus::Ignored;
        const bool final_fragment=static_cast<std::size_t>(header.fragment_index)+1==frame->count;
        if(!final_fragment&&payload.size()!=kVideoDataFragmentBytes)return ReassemblyStatus::Ignored;
        if(frame->type_known&&frame->type!=header.media_type)return ReassemblyStatus::Ignored;
        frame->type=header.media_type;frame->type_known=true;frame->flags|=header.flags;
        if(frame->type==VideoMediaType::VideoH264&&impl.awaiting_idr&&(frame->flags&(FrameKeyframe|FrameConfig))==0){
            if(!impl.has_inflight_keyframe_before(frame->id)){impl.release(*frame);return ReassemblyStatus::NeedIdr;}
        }
        const std::size_t index=header.fragment_index;
        if(!frame->fragment_present[index]){
            if(!payload.empty())std::memcpy(frame->fragment_ptr(index),payload.data(),payload.size());
            frame->fragment_lengths[index]=static_cast<std::uint16_t>(payload.size());
            frame->fragment_present[index]=1;
        }
    }

    for(std::size_t group=0;group<frame->groups();++group){
        if(!frame->parity_present[group])continue;
        const auto*parity=frame->parity_ptr(group);
        const std::size_t parity_size=frame->parity_lengths[group],start=group*10,count=parity[0];
        std::size_t missing=0,missing_index=0,longest=0;
        for(std::size_t j=0;j<count;++j){
            const std::size_t index=start+j;
            const std::size_t length=read16(parity+1+j*2);
            if(length>kVideoDataFragmentBytes)return ReassemblyStatus::Ignored;
            const bool final_fragment=index+1==frame->count;
            if(!final_fragment&&length!=kVideoDataFragmentBytes)return ReassemblyStatus::Ignored;
            longest=std::max(longest,length);
            if(!frame->fragment_present[index]){++missing;missing_index=index;}
        }
        if(kVideoFecMetadataBytes+longest>parity_size)return ReassemblyStatus::Ignored;
        if(missing==1){
            const std::size_t expected=read16(parity+1+(missing_index-start)*2);
            auto*dst=frame->fragment_ptr(missing_index);
            for(std::size_t k=0;k<expected;++k){
                std::uint8_t value=parity[kVideoFecMetadataBytes+k];
                for(std::size_t j=0;j<count;++j){
                    const std::size_t index=start+j;
                    if(index==missing_index||!frame->fragment_present[index]||k>=frame->fragment_lengths[index])continue;
                    value^=frame->fragment_ptr(index)[k];
                }
                dst[k]=value;
            }
            frame->fragment_lengths[missing_index]=static_cast<std::uint16_t>(expected);
            frame->fragment_present[missing_index]=1;
        }
    }

    frame->complete=frame->type_known&&std::all_of(frame->fragment_present.begin(),frame->fragment_present.end(),[](std::uint8_t present){return present!=0;});
    if(!frame->complete){
        if(need_idr)return ReassemblyStatus::NeedIdr;
        return impl.emit_ready_or(output,ReassemblyStatus::Incomplete);
    }

    if(frame->type!=VideoMediaType::VideoH264){impl.emit(*frame,output);return ReassemblyStatus::Complete;}

    const bool config=(frame->flags&FrameConfig)!=0;
    const bool keyframe=(frame->flags&FrameKeyframe)!=0;
    if(config){
        impl.release_older_video(frame->id);
        impl.awaiting_idr=true;
        impl.emit(*frame,output);
        return ReassemblyStatus::Complete;
    }
    if(keyframe){
        impl.release_older_video(frame->id);
        impl.emit(*frame,output);
        return ReassemblyStatus::Complete;
    }
    if(impl.awaiting_idr){
        if(impl.has_inflight_keyframe_before(frame->id))return ReassemblyStatus::Incomplete;
        impl.release(*frame);return ReassemblyStatus::NeedIdr;
    }

    if(impl.last_video_id&&frame->id==impl.last_video_id+1){impl.emit(*frame,output);return ReassemblyStatus::Complete;}

    // A complete future frame is not proof that its predecessor was lost. Keep it
    // in the existing fixed three-frame window; a later overflow is the bounded
    // point at which the receiver asks for a new IDR.
    if(need_idr)return ReassemblyStatus::NeedIdr;
    return impl.emit_ready_or(output,ReassemblyStatus::Incomplete);
}

ReassemblyStatus VideoReassembler::drain(ReassembledFrame&output){
    if(!impl_||!impl_->active)return ReassemblyStatus::Ignored;
    auto*frame=impl_->next_ready_video();
    if(!frame)return ReassemblyStatus::Incomplete;
    impl_->emit(*frame,output);
    return ReassemblyStatus::Complete;
}

std::size_t VideoReassembler::frames_in_flight()const{return impl_->count();}
std::size_t VideoReassembler::bytes_in_flight()const{return impl_->bytes;}
}
