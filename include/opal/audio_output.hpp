#pragma once

#include <cstdint>
#include <memory>
#include <span>

namespace opal {

class AudioOutput {
public:
    AudioOutput();
    AudioOutput(const AudioOutput&)=delete;
    AudioOutput& operator=(const AudioOutput&)=delete;
    bool configure_aac(std::span<const std::uint8_t> extradata,int sample_rate,int channels);
    bool submit(std::span<const std::uint8_t> aac,std::int64_t pts_us,
                std::int64_t current_video_pts_us);
    void reset_to(std::int64_t video_pts_us);
    std::uint32_t queued_ms() const;
    void close();
    ~AudioOutput();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
