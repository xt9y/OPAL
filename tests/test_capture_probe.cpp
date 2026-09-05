#include "capture_test_support.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

int main(){
    char pattern[]="/tmp/opal-ffmpeg-probe-XXXXXX";
    char*dir=mkdtemp(pattern);
    assert(dir);
    const std::filesystem::path root(dir);
    const auto ffmpeg=root/"ffmpeg";
    {
        std::ofstream out(ffmpeg);
        out<<"#!/bin/sh\n"
              "case \"$*\" in\n"
              "  *-encoders*) printf ' V..... libx264 H.264 encoder\\n'; exit 0 ;;\n"
              "  *) exit 1 ;;\n"
              "esac\n";
    }
    assert(chmod(ffmpeg.c_str(),0700)==0);
    const char*old_path=std::getenv("PATH");
    const std::string path=root.string()+":"+(old_path?old_path:"");
    assert(setenv("PATH",path.c_str(),1)==0);

    assert(opal_test::ffmpeg_h264_encoder().empty());

    std::filesystem::remove_all(root);
    return 0;
}
