#pragma once
#include <string>

namespace opal {
struct StreamOptions {
    int max_width=1920;
    int max_height=1080;
    int fps=60;
};

StreamOptions default_stream_options();
bool stream_mode_limit(const std::string &mode,int &max_width,int &max_height);
int automatic_bitrate_kbps(int width,int height,int fps);
}
