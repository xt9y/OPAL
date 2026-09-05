#include <opal/flv_stream.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

namespace {
void put24(std::vector<std::uint8_t>&out,std::uint32_t value){out.push_back(static_cast<std::uint8_t>(value>>16));out.push_back(static_cast<std::uint8_t>(value>>8));out.push_back(static_cast<std::uint8_t>(value));}
void put32(std::vector<std::uint8_t>&out,std::uint32_t value){out.push_back(static_cast<std::uint8_t>(value>>24));out.push_back(static_cast<std::uint8_t>(value>>16));out.push_back(static_cast<std::uint8_t>(value>>8));out.push_back(static_cast<std::uint8_t>(value));}
void tag(std::vector<std::uint8_t>&out,std::uint8_t type,std::uint32_t timestamp,const std::vector<std::uint8_t>&payload){out.push_back(type);put24(out,static_cast<std::uint32_t>(payload.size()));put24(out,timestamp);out.push_back(static_cast<std::uint8_t>(timestamp>>24));put24(out,0);out.insert(out.end(),payload.begin(),payload.end());put32(out,11u+static_cast<std::uint32_t>(payload.size()));}
}

int main(){
    std::vector<std::uint8_t> bytes={'F','L','V',1,5,0,0,0,9,0,0,0,0};
    tag(bytes,9,0,{0x17,0,0,0,0,1,0x64,0,0x1f});
    tag(bytes,8,0,{0xaf,0,0x12,0x10});
    tag(bytes,9,10,{0x17,1,0,0,5,0,0,0,2,0x65,0x88});
    tag(bytes,8,20,{0xaf,1,1,2,3});

    opal::FlvStreamParser parser;
    int video_config=0,audio_config=0,video=0,audio=0;
    for(std::size_t offset=0;offset<bytes.size();){
        const std::size_t count=std::min<std::size_t>((offset%7)+1,bytes.size()-offset);
        assert(parser.append(std::span<const std::uint8_t>(bytes.data()+offset,count)));
        offset+=count;
        for(;;){
            const auto event=parser.next();
            if(event.type==opal::FlvEventType::NeedMore)break;
            assert(event.type!=opal::FlvEventType::Invalid);
            if(event.type==opal::FlvEventType::VideoConfig){assert(event.data.size()==4);++video_config;}
            else if(event.type==opal::FlvEventType::AudioConfig){assert(event.sample_rate==44100&&event.channels==2);++audio_config;}
            else if(event.type==opal::FlvEventType::Video){assert(event.keyframe&&event.dts_us==10000&&event.pts_us==15000&&event.data.size()==6);++video;}
            else if(event.type==opal::FlvEventType::Audio){assert(event.pts_us==20000&&event.data.size()==3);++audio;}
        }
    }
    assert(video_config==1&&audio_config==1&&video==1&&audio==1);
    assert(parser.error().empty());

    auto corrupt=bytes;
    corrupt.back()^=1;
    opal::FlvStreamParser invalid;
    assert(invalid.append(corrupt));
    bool failed=false;
    for(;;){const auto event=invalid.next();if(event.type==opal::FlvEventType::Invalid){failed=true;break;}if(event.type==opal::FlvEventType::NeedMore)break;}
    assert(failed);
    return 0;
}
