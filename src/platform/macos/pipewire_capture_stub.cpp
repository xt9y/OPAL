#include <opal/pipewire_capture.hpp>

namespace opal {

struct NativePipeWireVideoCapture::Impl {};

NativePipeWireVideoCapture::NativePipeWireVideoCapture():impl_(std::make_unique<Impl>()){}
NativePipeWireVideoCapture::~NativePipeWireVideoCapture()=default;

bool NativePipeWireVideoCapture::start(const StreamOptions&,int,const std::string&){return false;}
bool NativePipeWireVideoCapture::next(EncodedMediaUnit&,int){return false;}
bool NativePipeWireVideoCapture::ended() const{return true;}
std::uint64_t NativePipeWireVideoCapture::config_revision() const{return 0;}
MediaConfig NativePipeWireVideoCapture::config() const{return {};}
std::string NativePipeWireVideoCapture::backend_name() const{return "unavailable-macos";}
std::string NativePipeWireVideoCapture::last_error() const{return "PipeWire capture is Linux-only; macOS host capture uses ScreenCaptureKit";}
void NativePipeWireVideoCapture::stop(){}
bool NativePipeWireVideoCapture::compiled(){return false;}

}
