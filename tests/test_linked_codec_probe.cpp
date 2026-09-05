#include "linked_codec_support.hpp"
#include <cstring>

int main(int argc,char**argv){
    const bool available=opal_test::linked_h264_decoder_available();
    if(argc>1&&std::strcmp(argv[1],"--check")==0)return available?0:1;
    if(!available)(void)opal_test::require_linked_h264_decoder();
    return 0;
}
