#include <opal/media_profile.hpp>
#include <cassert>

int main() {
    auto d=opal::default_stream_options();
    assert(d.max_width==1920);
    assert(d.max_height==1080);
    assert(d.fps==60);

    int w=-1,h=-1;
    assert(opal::stream_mode_limit("max",w,h)&&w==0&&h==0);
    assert(opal::stream_mode_limit("1080p",w,h)&&w==1920&&h==1080);
    assert(opal::stream_mode_limit("1440p",w,h)&&w==2560&&h==1440);
    assert(opal::stream_mode_limit("4k",w,h)&&w==3840&&h==2160);
    assert(!opal::stream_mode_limit("invalid",w,h));

    assert(opal::automatic_bitrate_kbps(1920,1080,60)==30000);
    return 0;
}
