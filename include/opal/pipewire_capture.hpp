#pragma once

#include <opal/media_profile.hpp>
#include <opal/multimonitor.hpp>
#include <opal/video_capture.hpp>
#include <cstdint>
#include <memory>
#include <string>

namespace opal {

bool native_pipewire_prepare(const StreamOptions& stream,
                             const std::string& restore_token_file,
                             std::string* error = nullptr);
CompositeLayout native_pipewire_layout();
bool native_pipewire_authorization_lost();
std::string native_pipewire_last_error();

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
