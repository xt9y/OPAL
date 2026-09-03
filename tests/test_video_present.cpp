#include <opal/video_present.hpp>
#include <cassert>
#include <cstring>
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

int main(){
    opal::VideoPresenter presenter;assert(presenter.open(320,180,false));assert(presenter.x11_window()!=0);auto size=presenter.drawable_size();assert(size.first>0&&size.second>0);
    assert(presenter.present(make_frame(48)));assert(presenter.pending_frame_count()<=1);
    assert(presenter.present(make_frame(96)));assert(presenter.pending_frame_count()<=1);
    assert(presenter.present(make_frame(160)));assert(presenter.pending_frame_count()<=1);
    presenter.close();assert(presenter.x11_window()==0);return 0;
}
