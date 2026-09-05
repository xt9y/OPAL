#include <opal/video_present.hpp>
#include <SDL3/SDL.h>
#include <cassert>
#include <cstring>
#include <string>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
}

static opal::DecodedVideoFrame make_yuv420(int value){
    AVFrame *frame=av_frame_alloc();assert(frame);frame->format=AV_PIX_FMT_YUV420P;frame->width=320;frame->height=180;assert(av_frame_get_buffer(frame,32)==0);assert(av_frame_make_writable(frame)==0);
    for(int y=0;y<frame->height;++y)std::memset(frame->data[0]+y*frame->linesize[0],value,frame->width);
    for(int y=0;y<frame->height/2;++y){std::memset(frame->data[1]+y*frame->linesize[1],128,frame->width/2);std::memset(frame->data[2]+y*frame->linesize[2],128,frame->width/2);}
    return{frame,value*1000};
}

static opal::DecodedVideoFrame make_nv12(int value){
    AVFrame *frame=av_frame_alloc();assert(frame);frame->format=AV_PIX_FMT_NV12;frame->width=320;frame->height=180;assert(av_frame_get_buffer(frame,32)==0);assert(av_frame_make_writable(frame)==0);
    for(int y=0;y<frame->height;++y)std::memset(frame->data[0]+y*frame->linesize[0],value,frame->width);
    for(int y=0;y<frame->height/2;++y){for(int x=0;x<frame->width;x+=2){frame->data[1][y*frame->linesize[1]+x]=128;frame->data[1][y*frame->linesize[1]+x+1]=128;}}
    return{frame,value*1000};
}

int main(){
    assert(opal::VideoPresenter::supports_cpu_upload_format(AV_PIX_FMT_YUV420P));
    assert(opal::VideoPresenter::supports_cpu_upload_format(AV_PIX_FMT_YUVJ420P));
    assert(opal::VideoPresenter::supports_cpu_upload_format(AV_PIX_FMT_NV12));
    assert(!opal::VideoPresenter::supports_cpu_upload_format(AV_PIX_FMT_DRM_PRIME));
    if(!SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS))return 0;
    opal::VideoPresenter presenter;if(!presenter.open(320,180,false,AV_PIX_FMT_YUV420P)){SDL_Quit();return 0;}
    assert(presenter.is_open());
    const auto backend=presenter.backend_name();
    assert(backend.find("sdl-renderer=")!=std::string::npos);
    assert(backend.find("opengl")==std::string::npos);
    assert(backend.find("pbo")==std::string::npos);
    assert(backend.find("dmabuf")==std::string::npos);
    const auto mode=presenter.presentation_mode();
    assert(mode=="immediate-active"||mode=="sdl-managed");
    assert(presenter.presented_frames()==0);
    auto size=presenter.drawable_size();assert(size.first>0&&size.second>0);

    auto borrowed=make_yuv420(48);AVFrame*borrowed_ptr=borrowed.frame;assert(presenter.present_borrowed({borrowed.frame,borrowed.pts_us}));assert(borrowed.frame==borrowed_ptr);assert(presenter.presented_frames()==1);av_frame_free(&borrowed.frame);
    assert(presenter.present(make_yuv420(96)));assert(presenter.presented_frames()==2);
    assert(presenter.present(make_nv12(64)));assert(presenter.pending_frame_count()==0);assert(presenter.presented_frames()==3);

    const bool captured=presenter.set_relative_mouse_mode(true);
    (void)presenter.set_relative_mouse_mode(false);
    (void)captured;
    presenter.close();assert(!presenter.is_open());
    SDL_Quit();return 0;
}
