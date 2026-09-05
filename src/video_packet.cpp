#include <opal/video_packet.hpp>
#include <algorithm>

namespace opal {
namespace {
void put16(std::uint8_t *p,std::uint16_t value){p[0]=static_cast<std::uint8_t>(value>>8);p[1]=static_cast<std::uint8_t>(value);}
void put32(std::uint8_t *p,std::uint32_t value){for(int i=0;i<4;++i)p[i]=static_cast<std::uint8_t>(value>>(24-8*i));}
void put64(std::uint8_t *p,std::uint64_t value){for(int i=0;i<8;++i)p[i]=static_cast<std::uint8_t>(value>>(56-8*i));}
std::uint16_t get16(const std::uint8_t *p){return static_cast<std::uint16_t>((p[0]<<8)|p[1]);}
std::uint32_t get32(const std::uint8_t *p){std::uint32_t v=0;for(int i=0;i<4;++i)v=(v<<8)|p[i];return v;}
std::uint64_t get64(const std::uint8_t *p){std::uint64_t v=0;for(int i=0;i<8;++i)v=(v<<8)|p[i];return v;}
bool valid_type(VideoMediaType type){const auto v=static_cast<unsigned>(type);return v>=1&&v<=6;}
}

bool use_fec_for_media(VideoMediaType type){return type==VideoMediaType::VideoH264;}

VideoFragmentCursor::VideoFragmentCursor(
    VideoMediaType type,std::uint16_t flags,std::uint32_t generation,std::uint64_t session_id,
    std::uint64_t frame_id,std::uint64_t capture_timestamp_us,std::span<const std::uint8_t> data,
    std::uint64_t &next_packet_sequence,bool fec)
    :type_(type),flags_(flags),generation_(generation),session_id_(session_id),frame_id_(frame_id),
     timestamp_(capture_timestamp_us),data_(data),sequence_(&next_packet_sequence),fec_(fec){
    count_=std::max<std::size_t>(1,(data_.size()+kVideoDataFragmentBytes-1)/kVideoDataFragmentBytes);
    valid_=count_<=65535&&valid_type(type_);
}

VideoFragmentCursor::VideoFragmentCursor(
    VideoMediaType type,std::uint16_t flags,std::uint32_t generation,std::uint64_t session_id,
    std::uint64_t frame_id,std::uint64_t capture_timestamp_us,std::span<const std::uint8_t> data,
    std::atomic<std::uint64_t> &next_packet_sequence,bool fec)
    :type_(type),flags_(flags),generation_(generation),session_id_(session_id),frame_id_(frame_id),
     timestamp_(capture_timestamp_us),data_(data),atomic_sequence_(&next_packet_sequence),fec_(fec){
    count_=std::max<std::size_t>(1,(data_.size()+kVideoDataFragmentBytes-1)/kVideoDataFragmentBytes);
    valid_=count_<=65535&&valid_type(type_);
}

std::uint64_t VideoFragmentCursor::take_sequence(){
    if(atomic_sequence_)return atomic_sequence_->fetch_add(1,std::memory_order_relaxed);
    return sequence_?(*sequence_)++:0;
}

bool VideoFragmentCursor::valid() const{return valid_&&(sequence_||atomic_sequence_);}

bool VideoFragmentCursor::next(VideoPacketHeader &header,std::span<const std::uint8_t> &payload){
    payload={};if(!valid())return false;
    if(emit_fec_){
        parity_.fill(0);
        const std::size_t group_count=std::min<std::size_t>(10,count_-fec_group_start_);
        parity_[0]=static_cast<std::uint8_t>(group_count);std::size_t longest=0;
        for(std::size_t j=0;j<group_count;++j){
            const std::size_t index=fec_group_start_+j,offset=index*kVideoDataFragmentBytes;
            const std::size_t length=offset<data_.size()?std::min(kVideoDataFragmentBytes,data_.size()-offset):0;
            put16(parity_.data()+1+j*2,static_cast<std::uint16_t>(length));longest=std::max(longest,length);
            for(std::size_t k=0;k<length;++k)parity_[kVideoFecMetadataBytes+k]^=data_[offset+k];
        }
        header={};header.media_type=VideoMediaType::Fec;header.flags=flags_;header.generation=generation_;
        header.session_id=session_id_;header.packet_sequence=take_sequence();header.frame_id=frame_id_;
        header.capture_timestamp_us=timestamp_;header.fragment_index=static_cast<std::uint16_t>(fec_group_start_);
        header.fragment_count=static_cast<std::uint16_t>(count_);header.fec_group=static_cast<std::uint16_t>(fec_group_start_/10);
        header.payload_length=static_cast<std::uint16_t>(kVideoFecMetadataBytes+longest);
        payload=std::span<const std::uint8_t>(parity_.data(),header.payload_length);emit_fec_=false;return true;
    }
    if(next_index_>=count_)return false;

    const std::size_t index=next_index_++,offset=index*kVideoDataFragmentBytes;
    const std::size_t length=offset<data_.size()?std::min(kVideoDataFragmentBytes,data_.size()-offset):0;
    header={};header.media_type=type_;header.flags=flags_|(index+1==count_?FrameEnd:0);
    header.generation=generation_;header.session_id=session_id_;header.packet_sequence=take_sequence();
    header.frame_id=frame_id_;header.capture_timestamp_us=timestamp_;header.fragment_index=static_cast<std::uint16_t>(index);
    header.fragment_count=static_cast<std::uint16_t>(count_);header.fec_group=static_cast<std::uint16_t>(index/10);
    header.payload_length=static_cast<std::uint16_t>(length);
    payload=length?data_.subspan(offset,length):std::span<const std::uint8_t>{};
    if(fec_&&(next_index_%10==0||next_index_==count_)){emit_fec_=true;fec_group_start_=index-(index%10);}
    return true;
}

std::array<std::uint8_t,kVideoHeaderBytes> serialize_video_header(const VideoPacketHeader &h){
    std::array<std::uint8_t,kVideoHeaderBytes> out{};
    put32(out.data(),h.magic);out[4]=h.version;out[5]=static_cast<std::uint8_t>(h.media_type);
    put16(out.data()+6,h.flags);put32(out.data()+8,h.generation);put64(out.data()+12,h.session_id);
    put64(out.data()+20,h.packet_sequence);put64(out.data()+28,h.frame_id);put64(out.data()+36,h.capture_timestamp_us);
    put16(out.data()+44,h.fragment_index);put16(out.data()+46,h.fragment_count);
    put16(out.data()+48,h.fec_group);put16(out.data()+50,h.payload_length);
    return out;
}

bool parse_video_header(std::span<const std::uint8_t> wire,VideoPacketHeader &h){
    if(wire.size()<kVideoHeaderBytes)return false;
    h.magic=get32(wire.data());h.version=wire[4];h.media_type=static_cast<VideoMediaType>(wire[5]);h.flags=get16(wire.data()+6);
    h.generation=get32(wire.data()+8);h.session_id=get64(wire.data()+12);h.packet_sequence=get64(wire.data()+20);
    h.frame_id=get64(wire.data()+28);h.capture_timestamp_us=get64(wire.data()+36);
    h.fragment_index=get16(wire.data()+44);h.fragment_count=get16(wire.data()+46);
    h.fec_group=get16(wire.data()+48);h.payload_length=get16(wire.data()+50);
    return h.magic==0x4f505631&&h.version==1&&valid_type(h.media_type)&&h.payload_length<=kVideoPlaintextBytes;
}

std::vector<std::uint8_t> serialize_plain_video_packet(const VideoPlainPacket &packet){
    if(packet.payload.size()>kVideoPlaintextBytes)return {};
    auto header=packet.header;header.payload_length=static_cast<std::uint16_t>(packet.payload.size());
    auto encoded=serialize_video_header(header);std::vector<std::uint8_t> wire;wire.reserve(encoded.size()+packet.payload.size());
    wire.insert(wire.end(),encoded.begin(),encoded.end());wire.insert(wire.end(),packet.payload.begin(),packet.payload.end());return wire;
}

bool parse_plain_video_packet(std::span<const std::uint8_t> wire,VideoPlainPacket &packet){
    VideoPacketHeader header;if(!parse_video_header(wire,header))return false;
    if(wire.size()!=kVideoHeaderBytes+header.payload_length)return false;
    packet.header=header;packet.payload.assign(wire.begin()+kVideoHeaderBytes,wire.end());return true;
}

std::vector<VideoPlainPacket> fragment_media_unit(
    VideoMediaType type,std::uint16_t flags,std::uint32_t generation,std::uint64_t session_id,
    std::uint64_t frame_id,std::uint64_t timestamp,std::span<const std::uint8_t> data,
    std::uint64_t &sequence,bool fec){
    const std::size_t count=std::max<std::size_t>(1,(data.size()+kVideoDataFragmentBytes-1)/kVideoDataFragmentBytes);
    if(count>65535)return {};
    std::vector<VideoPlainPacket> packets;packets.reserve(count+(fec?(count+9)/10:0));
    VideoFragmentCursor cursor(type,flags,generation,session_id,frame_id,timestamp,data,sequence,fec);
    if(!cursor.valid())return {};
    VideoPacketHeader header;std::span<const std::uint8_t> payload;
    while(cursor.next(header,payload))packets.push_back({header,std::vector<std::uint8_t>(payload.begin(),payload.end())});
    return packets;
}

}
