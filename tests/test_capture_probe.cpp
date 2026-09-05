#include "capture_test_support.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc,char**argv){
    if(argc==2&&std::string_view(argv[1])=="--check"){
        const auto encoder=opal_test::ffmpeg_h264_encoder();
        if(encoder.empty())return 1;
        std::cout<<encoder<<"\n";
        return 0;
    }
    if(argc!=1)return 2;

    char pattern[]="/tmp/opal-ffmpeg-probe-XXXXXX";
    char*dir=mkdtemp(pattern);
    assert(dir);
    const std::filesystem::path root(dir);
    const auto ffmpeg=root/"ffmpeg";
    {
        std::ofstream out(ffmpeg);
        out<<"#!/bin/sh\n"
              "case \" $* \" in\n"
              "  *' -c:v libx264 '*) exit 0 ;;\n"
              "  *' -c:v libopenh264 '*) printf 'FLV\\001\\005\\000\\000\\000\\011\\000\\000\\000\\000'; exit 0 ;;\n"
              "  *) exit 1 ;;\n"
              "esac\n";
    }
    assert(chmod(ffmpeg.c_str(),0700)==0);
    const char*old_path=std::getenv("PATH");
    const std::string path=root.string()+":"+(old_path?old_path:"");
    assert(setenv("PATH",path.c_str(),1)==0);

    assert(opal_test::ffmpeg_h264_encoder()=="libopenh264");

    std::filesystem::remove_all(root);
    return 0;
}
