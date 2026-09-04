#include <opal/video_reassembly.hpp>
#include <algorithm>
#include <cstring>
#include <map>

namespace opal {
namespace {
constexpr std::size_t kMaxBytes=16u*1024u*1024u;
constexpr std::size_t kMaxFragments=kMaxBytes/kVideoDataFragmentBytes+1;
std::uint16_t read16(const std::uint8_t*p){return static_cast<std::uint16_t>((p[0]<<8)|p[1]);}
std::size_t storage_for(std::size_t count){if(count==0||count>kMaxFragments)return 0;const std::size_t groups=(count+9)/10;const std::size_t data=count*kVideoDataFragmentBytes;const std::size_t meta=count*(sizeof(std::uint16_t)+sizeof(std::uint8_t));const std::size_t fec=groups*kVideoPlaintextBytes+groups*(sizeof(std::uint16_t)+sizeof(std::uint8_t));if(data>kMaxBytes||meta>kMaxBytes-data||fec>kMaxBytes-data-meta)return 0;return data+meta+fec;}
}

struct VideoReassembler::Impl{
    struct Frame{
        std::uint64_t id=0,timestamp=0,order=0,video_order=0;VideoMediaType type=VideoMediaType::VideoH264;bool type_known=false;std::uint16_t flags=0,count=0;std::vector<std::uint8_t> fragment_data,fragment_present,parity_data,parity_present;std::vector<std::uint16_t> fragment_lengths,parity_lengths;std::size_t storage_bytes=0;
        bool init(std::uint16_t fragments){count=fragments;storage_bytes=storage_for(count);if(!storage_bytes)return false;const std::size_t groups=(count+9)/10;fragment_data.resize(static_cast<std::size_t>(count)*kVideoDataFragmentBytes);fragment_present.assign(count,0);fragment_lengths.assign(count,0);parity_data.resize(groups*kVideoPlaintextBytes);parity_present.assign(groups,0);parity_lengths.assign(groups,0);return true;}
        std::size_t groups()const{return parity_present.size();}
        std::uint8_t*fragment_ptr(std::size_t index){return fragment_data.data()+index*kVideoDataFragmentBytes;}
        const std::uint8_t*fragment_ptr(std::size_t index)const{return fragment_data.data()+index*kVideoDataFragmentBytes;}
        std::uint8_t*parity_ptr(std::size_t group){return parity_data.data()+group*kVideoPlaintextBytes;}
        const std::uint8_t*parity_ptr(std::size_t group)const{return parity_data.data()+group*kVideoPlaintextBytes;}
    };
    std::uint32_t generation=0;std::uint64_t session_id=0,order=0,video_order=0,last_video_id=0;bool active=false,awaiting_idr=false;std::map<std::uint64_t,Frame> frames;std::size_t bytes=0;
};

VideoReassembler::VideoReassembler():impl_(std::make_unique<Impl>()){}VideoReassembler::~VideoReassembler()=default;
void VideoReassembler::reset(std::uint32_t generation,std::uint64_t session_id){impl_->generation=generation;impl_->session_id=session_id;impl_->active=true;impl_->frames.clear();impl_->bytes=0;impl_->order=0;impl_->video_order=0;impl_->last_video_id=0;impl_->awaiting_idr=true;}
void VideoReassembler::require_idr(){if(!impl_)return;impl_->awaiting_idr=true;for(auto it=impl_->frames.begin();it!=impl_->frames.end();){if(it->second.type_known&&it->second.type==VideoMediaType::VideoH264){impl_->bytes-=it->second.storage_bytes;it=impl_->frames.erase(it);}else ++it;}}
ReassemblyStatus VideoReassembler::accept(const VideoPlainPacket&p,ReassembledFrame&o){return accept(p.header,p.payload,o);}

ReassemblyStatus VideoReassembler::accept(const VideoPacketHeader&header,std::span<const std::uint8_t>payload,ReassembledFrame&output){
    auto&impl=*impl_;if(!impl.active||header.generation!=impl.generation||header.session_id!=impl.session_id)return ReassemblyStatus::Ignored;if(payload.size()!=header.payload_length||payload.size()>kVideoPlaintextBytes||header.fragment_count==0||header.fragment_count>kMaxFragments)return ReassemblyStatus::Ignored;const bool video_packet=header.media_type==VideoMediaType::VideoH264||header.media_type==VideoMediaType::Fec;if(video_packet&&impl.last_video_id&&header.frame_id<=impl.last_video_id)return ReassemblyStatus::Ignored;
    bool need_idr=false;auto frame_it=impl.frames.find(header.frame_id);
    if(frame_it==impl.frames.end()){
        const std::size_t planned=storage_for(header.fragment_count);if(!planned)return ReassemblyStatus::Ignored;
        while(!impl.frames.empty()&&(impl.frames.size()>=3||impl.bytes+planned>kMaxBytes)){auto victim=std::min_element(impl.frames.begin(),impl.frames.end(),[](const auto&a,const auto&b){return a.second.order<b.second.order;});if(victim==impl.frames.end())break;if(victim->second.type_known&&victim->second.type==VideoMediaType::VideoH264){need_idr=true;impl.awaiting_idr=true;}impl.bytes-=victim->second.storage_bytes;impl.frames.erase(victim);}
        if(impl.bytes+planned>kMaxBytes)return need_idr?ReassemblyStatus::NeedIdr:ReassemblyStatus::Ignored;
        Impl::Frame frame;frame.id=header.frame_id;frame.timestamp=header.capture_timestamp_us;frame.order=++impl.order;if(!frame.init(header.fragment_count))return ReassemblyStatus::Ignored;impl.bytes+=frame.storage_bytes;frame_it=impl.frames.emplace(frame.id,std::move(frame)).first;
    }
    auto&frame=frame_it->second;if(frame.count!=header.fragment_count)return ReassemblyStatus::Ignored;

    if(header.media_type==VideoMediaType::Fec){
        if(payload.size()<kVideoFecMetadataBytes||header.fec_group>=frame.groups())return ReassemblyStatus::Ignored;
        const std::size_t start=static_cast<std::size_t>(header.fec_group)*10,group_count=payload[0];
        if(group_count==0||group_count>10||start>=frame.count||group_count!=std::min<std::size_t>(10,frame.count-start))return ReassemblyStatus::Ignored;
        if(frame.type_known&&frame.type!=VideoMediaType::VideoH264)return ReassemblyStatus::Ignored;
        frame.type=VideoMediaType::VideoH264;frame.type_known=true;frame.flags|=header.flags;
        if(!frame.parity_present[header.fec_group]){std::memcpy(frame.parity_ptr(header.fec_group),payload.data(),payload.size());frame.parity_lengths[header.fec_group]=static_cast<std::uint16_t>(payload.size());frame.parity_present[header.fec_group]=1;}
    }else{
        if(header.fragment_index>=frame.count||payload.size()>kVideoDataFragmentBytes)return ReassemblyStatus::Ignored;
        if(frame.type_known&&frame.type!=header.media_type)return ReassemblyStatus::Ignored;
        frame.type=header.media_type;frame.type_known=true;frame.flags|=header.flags;
        if(frame.type==VideoMediaType::VideoH264&&(frame.flags&FrameConfig)==0&&frame.video_order==0){frame.video_order=++impl.video_order;for(auto it=impl.frames.begin();it!=impl.frames.end();){if(it==frame_it){++it;continue;}const auto&older=it->second;if(older.type_known&&older.type==VideoMediaType::VideoH264&&older.video_order>0&&frame.video_order>=older.video_order+2){impl.bytes-=older.storage_bytes;it=impl.frames.erase(it);need_idr=true;impl.awaiting_idr=true;}else ++it;}}
        if(frame.type==VideoMediaType::VideoH264&&impl.awaiting_idr&&(frame.flags&(FrameKeyframe|FrameConfig))==0){impl.bytes-=frame.storage_bytes;impl.frames.erase(frame_it);return ReassemblyStatus::NeedIdr;}
        const std::size_t index=header.fragment_index;if(!frame.fragment_present[index]){if(!payload.empty())std::memcpy(frame.fragment_ptr(index),payload.data(),payload.size());frame.fragment_lengths[index]=static_cast<std::uint16_t>(payload.size());frame.fragment_present[index]=1;}
    }

    for(std::size_t group=0;group<frame.groups();++group){if(!frame.parity_present[group])continue;const auto*parity=frame.parity_ptr(group);const std::size_t parity_size=frame.parity_lengths[group],start=group*10,count=parity[0];std::size_t missing=0,missing_index=0,longest=0;for(std::size_t j=0;j<count;++j){const std::size_t length=read16(parity+1+j*2);if(length>kVideoDataFragmentBytes)return ReassemblyStatus::Ignored;longest=std::max(longest,length);if(!frame.fragment_present[start+j]){++missing;missing_index=start+j;}}if(kVideoFecMetadataBytes+longest>parity_size)return ReassemblyStatus::Ignored;if(missing==1){const std::size_t expected=read16(parity+1+(missing_index-start)*2);auto*dst=frame.fragment_ptr(missing_index);for(std::size_t k=0;k<expected;++k){std::uint8_t value=parity[kVideoFecMetadataBytes+k];for(std::size_t j=0;j<count;++j){const std::size_t index=start+j;if(index==missing_index||!frame.fragment_present[index]||k>=frame.fragment_lengths[index])continue;value^=frame.fragment_ptr(index)[k];}dst[k]=value;}frame.fragment_lengths[missing_index]=static_cast<std::uint16_t>(expected);frame.fragment_present[missing_index]=1;}}

    const bool complete=frame.type_known&&std::all_of(frame.fragment_present.begin(),frame.fragment_present.end(),[](std::uint8_t present){return present!=0;});if(!complete)return need_idr?ReassemblyStatus::NeedIdr:ReassemblyStatus::Incomplete;
    if(frame.type==VideoMediaType::VideoH264&&(frame.flags&FrameConfig)==0&&impl.last_video_id&&frame.id>impl.last_video_id&&frame.id-impl.last_video_id>1&&(frame.flags&FrameKeyframe)==0){impl.awaiting_idr=true;impl.bytes-=frame.storage_bytes;impl.frames.erase(frame_it);return ReassemblyStatus::NeedIdr;}
    if(frame.type==VideoMediaType::VideoH264&&impl.awaiting_idr&&(frame.flags&(FrameKeyframe|FrameConfig))==0){impl.bytes-=frame.storage_bytes;impl.frames.erase(frame_it);return ReassemblyStatus::NeedIdr;}
    if(frame.type==VideoMediaType::VideoH264){
        if((frame.flags&(FrameKeyframe|FrameConfig))==0){
            bool older_pending=false;for(const auto&entry:impl.frames)if(entry.first<frame.id&&entry.second.type_known&&entry.second.type==VideoMediaType::VideoH264){older_pending=true;break;}
            if(older_pending){impl.awaiting_idr=true;for(auto it=impl.frames.begin();it!=impl.frames.end();){if(it->first<=frame.id&&it->second.type_known&&it->second.type==VideoMediaType::VideoH264){impl.bytes-=it->second.storage_bytes;it=impl.frames.erase(it);}else ++it;}return ReassemblyStatus::NeedIdr;}
        }else if((frame.flags&FrameKeyframe)!=0){
            for(auto it=impl.frames.begin();it!=impl.frames.end();){if(it==frame_it){++it;continue;}if(it->first<frame.id&&it->second.type_known&&it->second.type==VideoMediaType::VideoH264){impl.bytes-=it->second.storage_bytes;it=impl.frames.erase(it);}else ++it;}
        }
    }
    output.media_type=frame.type;output.flags=frame.flags;output.frame_id=frame.id;output.capture_timestamp_us=frame.timestamp;output.keyframe=(frame.flags&FrameKeyframe)!=0;output.config=(frame.flags&FrameConfig)!=0;output.data.clear();std::size_t total=0;for(auto length:frame.fragment_lengths)total+=length;output.data.reserve(total);for(std::size_t i=0;i<frame.count;++i)output.data.insert(output.data.end(),frame.fragment_ptr(i),frame.fragment_ptr(i)+frame.fragment_lengths[i]);if(frame.type==VideoMediaType::VideoH264){impl.last_video_id=std::max(impl.last_video_id,frame.id);if(output.keyframe)impl.awaiting_idr=false;}impl.bytes-=frame.storage_bytes;impl.frames.erase(frame_it);return ReassemblyStatus::Complete;
}

std::size_t VideoReassembler::frames_in_flight()const{return impl_->frames.size();}std::size_t VideoReassembler::bytes_in_flight()const{return impl_->bytes;}
}
