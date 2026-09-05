#include <opal/flv_stream.hpp>

#include <cstring>
#include <utility>

namespace opal {
namespace {
constexpr std::size_t kMaxFlvTagBytes=16u*1024u*1024u;

std::uint32_t be24(const std::uint8_t* p){return (std::uint32_t(p[0])<<16)|(std::uint32_t(p[1])<<8)|p[2];}
std::uint32_t be32(const std::uint8_t* p){return (std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|(std::uint32_t(p[2])<<8)|p[3];}
std::int32_t signed24(const std::uint8_t* p){std::uint32_t value=be24(p);if(value&0x800000u)value|=0xff000000u;return static_cast<std::int32_t>(value);}
bool fourcc(const std::uint8_t* p,char a,char b,char c,char d){return p[0]==static_cast<std::uint8_t>(a)&&p[1]==static_cast<std::uint8_t>(b)&&p[2]==static_cast<std::uint8_t>(c)&&p[3]==static_cast<std::uint8_t>(d);}

class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> data):data_(data){}
    bool get(unsigned bits,std::uint32_t& value){
        if(bits>32||bit_+bits>data_.size()*8)return false;
        value=0;
        for(unsigned i=0;i<bits;++i){value=(value<<1)|((data_[bit_/8]>>(7-(bit_%8)))&1u);++bit_;}
        return true;
    }
private:
    std::span<const std::uint8_t> data_;
    std::size_t bit_=0;
};

bool parse_aac_config(std::span<const std::uint8_t> data,int& sample_rate,int& channels){
    BitReader reader(data);std::uint32_t object=0,index=0,channel_config=0;
    if(!reader.get(5,object))return false;
    if(object==31){std::uint32_t extension=0;if(!reader.get(6,extension))return false;object=32+extension;}
    if(!reader.get(4,index))return false;
    static constexpr int rates[]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
    if(index==15){std::uint32_t explicit_rate=0;if(!reader.get(24,explicit_rate)||explicit_rate==0||explicit_rate>384000)return false;sample_rate=static_cast<int>(explicit_rate);}
    else{if(index>=13)return false;sample_rate=rates[index];}
    if(!reader.get(4,channel_config))return false;
    static constexpr int channel_count[]={0,1,2,3,4,5,6,8};
    if(channel_config>=8||channel_count[channel_config]==0)return false;
    channels=channel_count[channel_config];
    return object!=0;
}
}

void FlvStreamParser::reset(){buffer_.clear();offset_=0;header_ready_=false;error_.clear();}
void FlvStreamParser::fail(std::string message){if(error_.empty())error_=std::move(message);}

void FlvStreamParser::compact_for(std::size_t incoming){
    if(offset_==0)return;
    if(offset_==buffer_.size()){buffer_.clear();offset_=0;return;}
    if(buffer_.capacity()-buffer_.size()>=incoming&&offset_<256u*1024u)return;
    const auto remaining=buffer_.size()-offset_;
    std::memmove(buffer_.data(),buffer_.data()+offset_,remaining);
    buffer_.resize(remaining);offset_=0;
}

bool FlvStreamParser::append(std::span<const std::uint8_t> bytes){
    if(!error_.empty())return false;
    if(bytes.empty())return true;
    if(bytes.size()>kMaxFlvTagBytes){fail("FLV input chunk exceeds limit");return false;}
    compact_for(bytes.size());
    if(buffer_.size()+bytes.size()>kMaxFlvTagBytes+64u*1024u){fail("FLV buffer limit exceeded");return false;}
    buffer_.insert(buffer_.end(),bytes.begin(),bytes.end());return true;
}

bool FlvStreamParser::parse_header(){
    if(header_ready_)return true;
    if(buffer_.size()-offset_<9)return false;
    const auto* p=buffer_.data()+offset_;
    if(p[0]!='F'||p[1]!='L'||p[2]!='V'||p[3]!=1){fail("invalid FLV header");return false;}
    const std::uint32_t data_offset=be32(p+5);
    if(data_offset<9||data_offset>1024u*1024u){fail("invalid FLV data offset");return false;}
    if(buffer_.size()-offset_<static_cast<std::size_t>(data_offset)+4)return false;
    offset_+=data_offset;
    if(be32(buffer_.data()+offset_)!=0){fail("invalid FLV initial previous-tag size");return false;}
    offset_+=4;header_ready_=true;return true;
}

FlvEvent FlvStreamParser::next(){
    FlvEvent event;
    if(!error_.empty()){event.type=FlvEventType::Invalid;return event;}
    if(!parse_header()){if(!error_.empty())event.type=FlvEventType::Invalid;return event;}

    for(;;){
        if(buffer_.size()-offset_<11)return event;
        const auto* header=buffer_.data()+offset_;
        const std::uint8_t tag_type=header[0]&0x1f;
        const std::uint32_t payload_size=be24(header+1);
        if(payload_size>kMaxFlvTagBytes){fail("FLV tag exceeds limit");event.type=FlvEventType::Invalid;return event;}
        const std::size_t total=11u+payload_size+4u;
        if(buffer_.size()-offset_<total)return event;
        const std::uint32_t timestamp=be24(header+4)|(std::uint32_t(header[7])<<24);
        const auto* payload=header+11;
        if(be32(payload+payload_size)!=11u+payload_size){fail("invalid FLV previous-tag size");event.type=FlvEventType::Invalid;return event;}
        offset_+=total;

        if(tag_type==9){
            if(payload_size<1)continue;
            const std::uint8_t flags=payload[0];
            const int frame_type=(flags&0x70)>>4;
            if((flags&0x80)!=0){
                const std::uint8_t packet_type=flags&0x0f;
                if(packet_type==6||payload_size<5||!fourcc(payload+1,'a','v','c','1'))continue;
                std::size_t pos=5;
                if(packet_type==0){
                    const auto body=std::span<const std::uint8_t>(payload+pos,payload_size-pos);
                    if(body.empty()){fail("empty enhanced AVC configuration");event.type=FlvEventType::Invalid;return event;}
                    event.type=FlvEventType::VideoConfig;event.data=body;return event;
                }
                std::int64_t composition_us=0;
                if(packet_type==1){
                    if(payload_size<pos+3)continue;
                    composition_us=static_cast<std::int64_t>(signed24(payload+pos))*1000;
                    pos+=3;
                }else if(packet_type!=3)continue;
                const auto body=std::span<const std::uint8_t>(payload+pos,payload_size-pos);
                if(body.empty())continue;
                const std::int64_t dts=static_cast<std::int64_t>(timestamp)*1000;
                event.type=FlvEventType::Video;event.data=body;event.dts_us=dts;event.pts_us=dts+composition_us;event.keyframe=frame_type==1;return event;
            }

            if(payload_size<5)continue;
            const int codec=flags&0x0f;
            if(codec!=7)continue;
            const std::uint8_t packet_type=payload[1];
            const auto body=std::span<const std::uint8_t>(payload+5,payload_size-5);
            if(packet_type==0){if(body.empty()){fail("empty AVC configuration");event.type=FlvEventType::Invalid;return event;}event.type=FlvEventType::VideoConfig;event.data=body;return event;}
            if(packet_type!=1||body.empty())continue;
            const std::int64_t dts=static_cast<std::int64_t>(timestamp)*1000;
            event.type=FlvEventType::Video;event.data=body;event.dts_us=dts;event.pts_us=dts+static_cast<std::int64_t>(signed24(payload+2))*1000;event.keyframe=frame_type==1;return event;
        }

        if(tag_type==8){
            if(payload_size<1)continue;
            if((payload[0]&0xf0)==0x90){
                const std::uint8_t packet_type=payload[0]&0x0f;
                if(packet_type==5||payload_size<5||!fourcc(payload+1,'m','p','4','a'))continue;
                const auto body=std::span<const std::uint8_t>(payload+5,payload_size-5);
                if(packet_type==0){if(body.empty()||!parse_aac_config(body,event.sample_rate,event.channels)){fail("invalid enhanced AAC configuration");event.type=FlvEventType::Invalid;return event;}event.type=FlvEventType::AudioConfig;event.data=body;return event;}
                if(packet_type!=1||body.empty())continue;
                event.type=FlvEventType::Audio;event.data=body;event.pts_us=event.dts_us=static_cast<std::int64_t>(timestamp)*1000;return event;
            }

            if(payload_size<2||(payload[0]>>4)!=10)continue;
            const std::uint8_t packet_type=payload[1];
            const auto body=std::span<const std::uint8_t>(payload+2,payload_size-2);
            if(packet_type==0){if(body.empty()||!parse_aac_config(body,event.sample_rate,event.channels)){fail("invalid AAC configuration");event.type=FlvEventType::Invalid;return event;}event.type=FlvEventType::AudioConfig;event.data=body;return event;}
            if(packet_type!=1||body.empty())continue;
            event.type=FlvEventType::Audio;event.data=body;event.pts_us=event.dts_us=static_cast<std::int64_t>(timestamp)*1000;return event;
        }
    }
}

}
