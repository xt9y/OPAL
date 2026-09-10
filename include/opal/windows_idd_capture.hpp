#pragma once

#include <memory>

namespace opal {
class CaptureBackend;
std::unique_ptr<CaptureBackend> make_windows_idd_capture_backend();
}
