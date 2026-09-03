#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace opal {
constexpr std::size_t kVideoMaxDatagramBytes=1200;
constexpr std::size_t kVideoHeaderBytes=52;
constexpr std::size_t kVideoAeadTagBytes=16;
constexpr std::size_t kVideoPlaintextBytes=kVideoMaxDatagramBytes-kVideoHeaderBytes-kVideoAeadTagBytes;
constexpr std::size_t kVideoFecMetadataBytes=21;
constexpr std::size_t kVideoDataFragmentBytes=kVideoPlaintextBytes-kVideoFecMetadataBytes;

enum class VideoMediaType : std::uint8_t {
    VideoH264=1,AudioAac=2,Probe=3,ProbeAck=4,Fec=5,Keepalive=6
};
enum VideoFrameFlags : std::uint16_t {
    FrameKeyframe=1u,FrameConfig=2u,FrameEnd=4u
};

struct VideoPacketHeader {
    std::uint32_t magic=0x4f505631;
    std::uint8_t version=1;
    VideoMediaType media_type=VideoMediaType::VideoH264;
    std::uint16_t flags=0;
    std::uint32_t generation=0;
    std::uint64_t session_id=0,packet_sequence=0,frame_id=0,capture_timestamp_us=0;
    std::uint16_t fragment_index=0,fragment_count=0,fec_group=0,payload_length=0;
};

struct VideoPlainPacket {
    VideoPacketHeader header;
    std::vector<std::uint8_t> payload;
};

class VideoFragmentCursor {
public:
    VideoFragmentCursor(VideoMediaType,std::uint16_t flags,std::uint32_t generation,
        std::uint64_t session_id,std::uint64_t frame_id,std::uint64_t capture_timestamp_us,
        std::span<const std::uint8_t> data,std::uint64_t &next_packet_sequence,bool fec=true);
    bool valid() const;
    bool next(VideoPacketHeader&,std::span<const std::uint8_t>&);
private:
    VideoMediaType type_=VideoMediaType::VideoH264;
    std::uint16_t flags_=0;
    std::uint32_t generation_=0;
    std::uint64_t session_id_=0,frame_id_=0,timestamp_=0;
    std::span<const std::uint8_t> data_{};
    std::uint64_t *sequence_=nullptr;
    std::size_t count_=0,next_index_=0,fec_group_start_=0;
    bool fec_=false,emit_fec_=false,valid_=false;
    std::array<std::uint8_t,kVideoPlaintextBytes> parity_{};
};

bool use_fec_for_media(VideoMediaType);
std::array<std::uint8_t,kVideoHeaderBytes> serialize_video_header(const VideoPacketHeader&);
bool parse_video_header(std::span<const std::uint8_t>,VideoPacketHeader&);
std::vector<std::uint8_t> serialize_plain_video_packet(const VideoPlainPacket&);
bool parse_plain_video_packet(std::span<const std::uint8_t>,VideoPlainPacket&);
std::vector<VideoPlainPacket> fragment_media_unit(
    VideoMediaType,std::uint16_t flags,std::uint32_t generation,std::uint64_t session_id,
    std::uint64_t frame_id,std::uint64_t capture_timestamp_us,std::span<const std::uint8_t>,
    std::uint64_t &next_packet_sequence,bool fec=true);
}
