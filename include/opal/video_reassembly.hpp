#pragma once
#include <opal/video_packet.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace opal {
enum class ReassemblyStatus { Incomplete, Complete, NeedIdr, Ignored };

struct ReassembledFrame {
    VideoMediaType media_type=VideoMediaType::VideoH264;
    std::uint16_t flags=0;
    std::uint64_t frame_id=0,capture_timestamp_us=0;
    bool keyframe=false,config=false;
    std::vector<std::uint8_t> data;
};

class VideoReassembler {
public:
    VideoReassembler();
    ~VideoReassembler();
    VideoReassembler(const VideoReassembler&)=delete;
    VideoReassembler& operator=(const VideoReassembler&)=delete;
    void reset(std::uint32_t generation,std::uint64_t session_id);
    ReassemblyStatus accept(const VideoPlainPacket&,ReassembledFrame&);
    std::size_t frames_in_flight() const;
    std::size_t bytes_in_flight() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
