#pragma once

#include <opal/media_profile.hpp>
#include <opal/video_capture.hpp>
#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace opal {

class NativePipeWireVideoCapture {
public:
    NativePipeWireVideoCapture();
    NativePipeWireVideoCapture(const NativePipeWireVideoCapture&)=delete;
    NativePipeWireVideoCapture& operator=(const NativePipeWireVideoCapture&)=delete;
    bool start(const StreamOptions& stream,int bitrate_kbps,const std::string& restore_token_file);
    bool next(EncodedMediaUnit& unit,int timeout_ms);
    bool ended() const;
    std::uint64_t config_revision() const;
    MediaConfig config() const;
    std::string backend_name() const;
    std::string last_error() const;
    void stop();
    ~NativePipeWireVideoCapture();
    static bool compiled();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
