#include <opal/video_present.hpp>
#include <SDL3/SDL.h>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
}

static opal::DecodedVideoFrame make_frame(int value){AVFrame *frame=av_frame_alloc();assert(frame);frame->format=AV_PIX_FMT_YUV420P;frame->width=320;frame->height=180;assert(av_frame_get_buffer(frame,32)==0);assert(av_frame_make_writable(frame)==0);for(int y=0;y<frame->height;++y)std::memset(frame->data[0]+y*frame->linesize[0],value,frame->width);for(int y=0;y<frame->height/2;++y){std::memset(frame->data[1]+y*frame->linesize[1],128,frame->width/2);std::memset(frame->data[2]+y*frame->linesize[2],128,frame->width/2);}return{frame,value*1000};}
static std::string read_all(const char*path){std::ifstream in(path);assert(in.good());return std::string(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());}

int main(){
    const auto source=read_all("src/video_present.cpp");const auto header=read_all("include/opal/video_present.hpp");
    assert(source.find("<SDL3/SDL.h>")!=std::string::npos);
    assert(source.find("SDL_CreateWindow")!=std::string::npos);
    assert(source.find("SDL_GL_CreateContext")!=std::string::npos);
    assert(source.find("SDL_GL_SetSwapInterval(0)")!=std::string::npos);
    assert(source.find("SDL_GL_SwapWindow")!=std::string::npos);
    assert(source.find("SDL_GetCurrentVideoDriver")!=std::string::npos);
    assert(source.find("SDL_SetWindowRelativeMouseMode")!=std::string::npos);
    assert(source.find("glTexSubImage2D")!=std::string::npos);
    assert(source.find("GL_UNPACK_ROW_LENGTH")!=std::string::npos);
    assert(source.find("uniform sampler2D tex_uv")!=std::string::npos);
    assert(source.find("glX") == std::string::npos);
    assert(source.find("XOpenDisplay") == std::string::npos);
    assert(header.find("X11/") == std::string::npos);
    assert(header.find("x11_window") == std::string::npos);

    if(!SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS))return 0;
    opal::VideoPresenter presenter;if(!presenter.open(320,180,false)){SDL_Quit();return 0;}
    assert(presenter.is_open());assert(!presenter.backend_name().empty());assert(presenter.presented_frames()==0);auto size=presenter.drawable_size();assert(size.first>0&&size.second>0);
    auto borrowed=make_frame(48);AVFrame*borrowed_ptr=borrowed.frame;assert(presenter.present_borrowed({borrowed.frame,borrowed.pts_us}));assert(borrowed.frame==borrowed_ptr);assert(presenter.presented_frames()==1);av_frame_free(&borrowed.frame);
    assert(presenter.present(make_frame(96)));assert(presenter.pending_frame_count()==0);assert(presenter.presented_frames()==2);
    presenter.close();assert(!presenter.is_open());SDL_Quit();return 0;
}
