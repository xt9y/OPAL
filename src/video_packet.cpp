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
bool valid_type(VideoMediaType type){const auto v=static_cast<unsigned>(type);return v>=1&&v<=5;}
}

bool use_fec_for_media(VideoMediaType type){return type==VideoMediaType::VideoH264;}

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
    for(std::size_t start=0;start<count;start+=10){
        const std::size_t group_count=std::min<std::size_t>(10,count-start);
        std::vector<std::vector<std::uint8_t>> group;group.reserve(group_count);std::size_t longest=0;
        for(std::size_t j=0;j<group_count;++j){
            const std::size_t index=start+j,offset=index*kVideoDataFragmentBytes;
            const std::size_t length=offset<data.size()?std::min(kVideoDataFragmentBytes,data.size()-offset):0;
            std::vector<std::uint8_t> payload;if(length)payload.assign(data.begin()+offset,data.begin()+offset+length);
            longest=std::max(longest,length);group.push_back(payload);
            VideoPacketHeader header;header.media_type=type;header.flags=flags|(index+1==count?FrameEnd:0);
            header.generation=generation;header.session_id=session_id;header.packet_sequence=sequence++;
            header.frame_id=frame_id;header.capture_timestamp_us=timestamp;header.fragment_index=static_cast<std::uint16_t>(index);
            header.fragment_count=static_cast<std::uint16_t>(count);header.fec_group=static_cast<std::uint16_t>(start/10);
            header.payload_length=static_cast<std::uint16_t>(length);packets.push_back({header,std::move(payload)});
        }
        if(fec){
            std::vector<std::uint8_t> parity(kVideoFecMetadataBytes+longest,0);parity[0]=static_cast<std::uint8_t>(group_count);
            for(std::size_t j=0;j<group_count;++j){
                put16(parity.data()+1+j*2,static_cast<std::uint16_t>(group[j].size()));
                for(std::size_t k=0;k<group[j].size();++k)parity[kVideoFecMetadataBytes+k]^=group[j][k];
            }
            VideoPacketHeader header;header.media_type=VideoMediaType::Fec;header.flags=flags;header.generation=generation;
            header.session_id=session_id;header.packet_sequence=sequence++;header.frame_id=frame_id;header.capture_timestamp_us=timestamp;
            header.fragment_index=static_cast<std::uint16_t>(start);header.fragment_count=static_cast<std::uint16_t>(count);
            header.fec_group=static_cast<std::uint16_t>(start/10);header.payload_length=static_cast<std::uint16_t>(parity.size());
            packets.push_back({header,std::move(parity)});
        }
    }
    return packets;
}
}
