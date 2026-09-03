#include <opal/video_packet.hpp>
#include <opal/video_reassembly.hpp>
#include <cassert>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static std::vector<opal::VideoPlainPacket> make_frame(std::uint64_t id,std::size_t bytes,std::uint16_t flags=0){std::vector<std::uint8_t> data(bytes);for(std::size_t i=0;i<data.size();++i)data[i]=static_cast<std::uint8_t>((i+id*13u)&0xffu);std::uint64_t sequence=id*1000;return opal::fragment_media_unit(opal::VideoMediaType::VideoH264,flags,5,77,id,id*16667,data,sequence,true);}
static std::string read_all(const char*path){std::ifstream in(path);return std::string(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());}

int main(){
    const auto source=read_all("src/video_reassembly.cpp");
    assert(source.find("optional<std::vector") == std::string::npos);
    assert(source.find("map<std::uint16_t,std::vector") == std::string::npos);
    assert(source.find("fragment_data") != std::string::npos);

    std::vector<std::uint8_t> expected(100*1024);for(std::size_t i=0;i<expected.size();++i)expected[i]=static_cast<std::uint8_t>((i+13u)&0xffu);std::uint64_t sequence=1000;auto packets=opal::fragment_media_unit(opal::VideoMediaType::VideoH264,opal::FrameKeyframe,5,77,1,16667,expected,sequence,true);opal::VideoReassembler reassembler;reassembler.reset(5,77);opal::ReassembledFrame complete;bool completed=false;for(const auto &packet:packets){if(packet.header.media_type!=opal::VideoMediaType::Fec&&packet.header.fragment_index==3)continue;auto status=reassembler.accept(packet,complete);if(status==opal::ReassemblyStatus::Complete){completed=true;break;}}assert(completed&&complete.data==expected&&complete.frame_id==1&&complete.keyframe);

    reassembler.reset(5,77);completed=false;for(const auto &packet:packets){if(packet.header.media_type!=opal::VideoMediaType::Fec&&(packet.header.fragment_index==2||packet.header.fragment_index==3))continue;if(reassembler.accept(packet,complete)==opal::ReassemblyStatus::Complete)completed=true;}assert(!completed);

    reassembler.reset(5,77);auto lost_reference=make_frame(50,opal::kVideoDataFragmentBytes+100);assert(reassembler.accept(lost_reference.front(),complete)==opal::ReassemblyStatus::Incomplete);auto next_reference=make_frame(51,500);bool next_complete=false;for(const auto &packet:next_reference)if(reassembler.accept(packet,complete)==opal::ReassemblyStatus::Complete)next_complete=true;assert(next_complete);auto stale_edge=make_frame(52,500);bool need_idr=false;for(const auto &packet:stale_edge)if(reassembler.accept(packet,complete)==opal::ReassemblyStatus::NeedIdr)need_idr=true;assert(need_idr);auto dependent=make_frame(53,500);bool dependent_complete=false;for(const auto &packet:dependent)if(reassembler.accept(packet,complete)==opal::ReassemblyStatus::Complete)dependent_complete=true;assert(!dependent_complete);auto recovery=make_frame(54,500,opal::FrameKeyframe);bool recovery_complete=false;for(const auto &packet:recovery){if(reassembler.accept(packet,complete)==opal::ReassemblyStatus::Complete){recovery_complete=true;assert(complete.keyframe);}}assert(recovery_complete);

    reassembler.reset(5,77);opal::ReassemblyStatus fourth=opal::ReassemblyStatus::Incomplete;for(std::uint64_t id=10;id<14;++id){auto frame=make_frame(id,opal::kVideoDataFragmentBytes+100);for(const auto &packet:frame){if(packet.header.media_type!=opal::VideoMediaType::Fec&&packet.header.fragment_index==0){fourth=reassembler.accept(packet,complete);break;}}}assert(reassembler.frames_in_flight()<=3);assert(reassembler.bytes_in_flight()<=16u*1024u*1024u);assert(fourth==opal::ReassemblyStatus::NeedIdr);

    reassembler.reset(5,77);opal::VideoPlainPacket oversized;oversized.header.generation=5;oversized.header.session_id=77;oversized.header.frame_id=20;oversized.header.fragment_count=1;oversized.header.payload_length=static_cast<std::uint16_t>(opal::kVideoPlaintextBytes);oversized.payload.resize(opal::kVideoPlaintextBytes+1);assert(reassembler.accept(oversized,complete)==opal::ReassemblyStatus::Ignored);assert(reassembler.bytes_in_flight()==0);

    auto old=make_frame(30,opal::kVideoDataFragmentBytes+10);for(const auto &packet:old){if(packet.header.media_type!=opal::VideoMediaType::Fec){reassembler.accept(packet,complete);break;}}assert(reassembler.frames_in_flight()==1);reassembler.reset(6,88);assert(reassembler.frames_in_flight()==0&&reassembler.bytes_in_flight()==0);assert(reassembler.accept(old.front(),complete)==opal::ReassemblyStatus::Ignored);return 0;
}
