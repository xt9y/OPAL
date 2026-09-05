#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace opal {

enum class FlvEventType {
    NeedMore,
    VideoConfig,
    AudioConfig,
    Video,
    Audio,
    Invalid
};

struct FlvEvent {
    FlvEventType type=FlvEventType::NeedMore;
    std::span<const std::uint8_t> data{};
    std::int64_t pts_us=0;
    std::int64_t dts_us=0;
    bool keyframe=false;
    int sample_rate=0;
    int channels=0;
};

class FlvStreamParser {
public:
    void reset();
    bool append(std::span<const std::uint8_t> bytes);
    FlvEvent next();
    const std::string& error() const { return error_; }
    std::size_t buffered_bytes() const { return buffer_.size()-offset_; }

private:
    bool parse_header();
    void fail(std::string message);
    void compact_for(std::size_t incoming);

    std::vector<std::uint8_t> buffer_;
    std::size_t offset_=0;
    bool header_ready_=false;
    std::string error_;
};

}
