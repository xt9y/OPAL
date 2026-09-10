#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace opal {

class CaptureBackend;
class VideoEncoderBackend;

std::unique_ptr<CaptureBackend> make_linux_pipewire_capture_backend(std::uint32_t node_id);
std::unique_ptr<VideoEncoderBackend> make_linux_video_encoder_backend();

}
