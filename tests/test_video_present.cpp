#include <opal/video_present.hpp>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
}

static opal::DecodedVideoFrame make_frame(int value){
    AVFrame *frame=av_frame_alloc();assert(frame);frame->format=AV_PIX_FMT_YUV420P;frame->width=320;frame->height=180;assert(av_frame_get_buffer(frame,32)==0);assert(av_frame_make_writable(frame)==0);
    for(int y=0;y<frame->height;++y)std::memset(frame->data[0]+y*frame->linesize[0],value,frame->width);
    for(int y=0;y<frame->height/2;++y){std::memset(frame->data[1]+y*frame->linesize[1],128,frame->width/2);std::memset(frame->data[2]+y*frame->linesize[2],128,frame->width/2);}
    return {frame,value*1000};
}

static std::string read_all(const char *path){
    std::ifstream in(path);assert(in.good());
    return std::string(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());
}

int main(){
    const auto source=read_all("src/video_present.cpp");
    assert(source.find("glXSwapIntervalEXT")!=std::string::npos||source.find("glXSwapIntervalMESA")!=std::string::npos);
    assert(source.find("_NET_WM_BYPASS_COMPOSITOR")!=std::string::npos);
    assert(source.find("glTexSubImage2D")!=std::string::npos);
    assert(source.find("GL_UNPACK_ROW_LENGTH")!=std::string::npos);
    assert(source.find("uniform sampler2D tex_uv")!=std::string::npos);
    assert(source.find("presented_frames")!=std::string::npos);
    assert(source.find("std::vector<std::uint8_t> scratch_y,scratch_u,scratch_v,scratch_uv") == std::string::npos);

    opal::VideoPresenter presenter;assert(presenter.open(320,180,false));assert(presenter.x11_window()!=0);assert(presenter.presented_frames()==0);auto size=presenter.drawable_size();assert(size.first>0&&size.second>0);
    auto borrowed=make_frame(48);AVFrame *borrowed_ptr=borrowed.frame;
    assert(presenter.present_borrowed({borrowed.frame,borrowed.pts_us}));
    assert(borrowed.frame==borrowed_ptr); // presenter must not take decoder-owned frame ownership
    assert(presenter.presented_frames()==1);
    av_frame_free(&borrowed.frame);
    assert(presenter.present(make_frame(96)));assert(presenter.pending_frame_count()<=1);
    assert(presenter.present(make_frame(160)));assert(presenter.pending_frame_count()<=1);assert(presenter.presented_frames()==3);
    presenter.close();assert(presenter.x11_window()==0);return 0;
}
