#include <opal/video_present.hpp>
#include <opal/latency_window.hpp>

#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

namespace opal {
namespace {
using Clock=std::chrono::steady_clock;
bool debug_enabled(){const char*v=std::getenv("OPAL_DEBUG");return v&&*v&&std::string(v)!="0";}
double elapsed_ms(Clock::time_point begin,Clock::time_point end){return std::chrono::duration<double,std::milli>(end-begin).count();}
SDL_FRect fitted_rect(int ww,int wh,int sw,int sh){ww=std::max(1,ww);wh=std::max(1,wh);sw=std::max(1,sw);sh=std::max(1,sh);const double source=static_cast<double>(sw)/static_cast<double>(sh),window=static_cast<double>(ww)/static_cast<double>(wh);float w=static_cast<float>(ww),h=static_cast<float>(wh);if(window>source)w=static_cast<float>(static_cast<double>(wh)*source);else if(window<source)h=static_cast<float>(static_cast<double>(ww)/source);return SDL_FRect{(static_cast<float>(ww)-w)*0.5f,(static_cast<float>(wh)-h)*0.5f,w,h};}
}

struct VideoPresenter::Impl{
    SDL_Window*window=nullptr;
    SDL_Renderer*renderer=nullptr;
    SDL_Texture*texture=nullptr;
    AVFrame*transfer_frame=nullptr;
    std::string renderer_name="unconfigured";
    int texture_format=AV_PIX_FMT_NONE,texture_width=0,texture_height=0;
    int drawable_width=0,drawable_height=0;
    std::uint64_t presented=0,last_size_refresh=0;
    bool immediate=false;
    LatencyWindow<128>upload_latency,present_latency;
    Clock::time_point last_debug{};

    bool refresh_drawable_size(bool force=false){if(!window)return false;if(!force&&drawable_width>0&&drawable_height>0&&presented-last_size_refresh<8)return true;int w=0,h=0;if(!SDL_GetWindowSizeInPixels(window,&w,&h)||w<=0||h<=0)return drawable_width>0&&drawable_height>0;drawable_width=w;drawable_height=h;last_size_refresh=presented;return true;}
    bool init_renderer(){renderer=SDL_CreateRenderer(window,nullptr);if(!renderer)return false;const char*name=SDL_GetRendererName(renderer);renderer_name=name&&*name?name:"unknown";(void)SDL_SetRenderVSync(renderer,SDL_RENDERER_VSYNC_DISABLED);int vsync=1;immediate=SDL_GetRenderVSync(renderer,&vsync)&&vsync==SDL_RENDERER_VSYNC_DISABLED;(void)SDL_SetRenderDrawColor(renderer,0,0,0,255);return true;}
    bool ensure_texture(int width,int height,int format){if(!renderer||width<=0||height<=0)return false;if(texture&&texture_width==width&&texture_height==height&&texture_format==format)return true;if(texture){SDL_DestroyTexture(texture);texture=nullptr;}SDL_PixelFormat pixel_format=SDL_PIXELFORMAT_UNKNOWN;SDL_Colorspace colorspace=SDL_COLORSPACE_BT709_LIMITED;if(format==AV_PIX_FMT_YUV420P){pixel_format=SDL_PIXELFORMAT_IYUV;}else if(format==AV_PIX_FMT_YUVJ420P){pixel_format=SDL_PIXELFORMAT_IYUV;colorspace=SDL_COLORSPACE_BT709_FULL;}else if(format==AV_PIX_FMT_NV12){pixel_format=SDL_PIXELFORMAT_NV12;}else return false;const SDL_PropertiesID props=SDL_CreateProperties();if(!props)return false;const bool ok=SDL_SetNumberProperty(props,SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,static_cast<Sint64>(pixel_format))&&SDL_SetNumberProperty(props,SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,static_cast<Sint64>(SDL_TEXTUREACCESS_STREAMING))&&SDL_SetNumberProperty(props,SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER,width)&&SDL_SetNumberProperty(props,SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER,height)&&SDL_SetNumberProperty(props,SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER,static_cast<Sint64>(colorspace));if(ok)texture=SDL_CreateTextureWithProperties(renderer,props);SDL_DestroyProperties(props);if(!texture)return false;(void)SDL_SetTextureScaleMode(texture,SDL_SCALEMODE_LINEAR);texture_width=width;texture_height=height;texture_format=format;return true;}
    bool normalize_frame(const AVFrame*source,const AVFrame*&normalized){normalized=source;if(!source)return false;if(source->format!=AV_PIX_FMT_DRM_PRIME)return VideoPresenter::supports_cpu_upload_format(source->format);if(!transfer_frame)transfer_frame=av_frame_alloc();if(!transfer_frame)return false;av_frame_unref(transfer_frame);if(av_hwframe_transfer_data(transfer_frame,source,0)<0)return false;if(av_frame_copy_props(transfer_frame,source)<0){av_frame_unref(transfer_frame);return false;}normalized=transfer_frame;return VideoPresenter::supports_cpu_upload_format(normalized->format);}
    bool present_frame(const AVFrame*source){const AVFrame*f=nullptr;if(!normalize_frame(source,f)||!f||!ensure_texture(f->width,f->height,f->format)||!refresh_drawable_size())return false;const auto upload_begin=Clock::now();bool uploaded=false;if(f->format==AV_PIX_FMT_YUV420P||f->format==AV_PIX_FMT_YUVJ420P)uploaded=SDL_UpdateYUVTexture(texture,nullptr,f->data[0],f->linesize[0],f->data[1],f->linesize[1],f->data[2],f->linesize[2]);else if(f->format==AV_PIX_FMT_NV12)uploaded=SDL_UpdateNVTexture(texture,nullptr,f->data[0],f->linesize[0],f->data[1],f->linesize[1]);const auto upload_end=Clock::now();if(!uploaded)return false;upload_latency.push(elapsed_ms(upload_begin,upload_end));const SDL_FRect dst=fitted_rect(drawable_width,drawable_height,f->width,f->height);const auto present_begin=Clock::now();if(!SDL_RenderClear(renderer)||!SDL_RenderTexture(renderer,texture,nullptr,&dst)||!SDL_RenderPresent(renderer))return false;const auto present_end=Clock::now();present_latency.push(elapsed_ms(present_begin,present_end));++presented;debug_timing();return true;}
    void debug_timing(){if(!debug_enabled())return;const auto now=Clock::now();if(last_debug.time_since_epoch().count()!=0&&now-last_debug<std::chrono::seconds(1))return;last_debug=now;const auto upload=upload_latency.snapshot(),present=present_latency.snapshot();const char*path=texture_format==AV_PIX_FMT_NV12?"sdl-nv12":"sdl-yuv";std::cerr<<"OPAL present upload_submit p50="<<upload.p50<<"ms p95="<<upload.p95<<"ms p99="<<upload.p99<<"ms render_present p50="<<present.p50<<"ms p95="<<present.p95<<"ms p99="<<present.p99<<"ms path="<<path<<" presentation="<<(immediate?"immediate-active":"sdl-managed")<<" photon_timing=unmeasured\n";}
};

VideoPresenter::VideoPresenter():impl_(std::make_unique<Impl>()){}
bool VideoPresenter::supports_cpu_upload_format(int format){return format==AV_PIX_FMT_YUV420P||format==AV_PIX_FMT_YUVJ420P||format==AV_PIX_FMT_NV12;}
bool VideoPresenter::open(int sw,int sh,bool fullscreen,int source_format){close();if(sw<=0||sh<=0)return false;impl_=std::make_unique<Impl>();SDL_WindowFlags flags=SDL_WINDOW_RESIZABLE|SDL_WINDOW_HIGH_PIXEL_DENSITY;if(fullscreen)flags|=SDL_WINDOW_FULLSCREEN;impl_->window=SDL_CreateWindow("OPAL",sw,sh,flags);if(!impl_->window){close();return false;}if(!impl_->init_renderer()){close();return false;}if(source_format>=0&&source_format!=AV_PIX_FMT_DRM_PRIME&&!impl_->ensure_texture(sw,sh,source_format)){close();return false;}if(!impl_->refresh_drawable_size(true)){close();return false;}return true;}
bool VideoPresenter::present_borrowed(DecodedVideoView decoded){return decoded.frame&&impl_&&impl_->window&&impl_->renderer&&impl_->present_frame(decoded.frame);}
bool VideoPresenter::present(DecodedVideoFrame decoded){AVFrame*f=decoded.frame;if(!f)return false;const bool ok=present_borrowed({f,decoded.pts_us});av_frame_free(&f);return ok;}
std::pair<int,int> VideoPresenter::drawable_size()const{if(!impl_||!impl_->window)return{0,0};if(impl_->drawable_width>0&&impl_->drawable_height>0)return{impl_->drawable_width,impl_->drawable_height};int w=0,h=0;if(!SDL_GetWindowSizeInPixels(impl_->window,&w,&h))return{0,0};return{w,h};}
std::pair<int,int> VideoPresenter::window_size()const{if(!impl_||!impl_->window)return{0,0};int w=0,h=0;if(!SDL_GetWindowSize(impl_->window,&w,&h))return{0,0};return{w,h};}
bool VideoPresenter::set_relative_mouse_mode(bool enabled){if(!impl_||!impl_->window)return false;if(!SDL_SetWindowRelativeMouseMode(impl_->window,enabled))return false;if(!SDL_SetWindowMouseGrab(impl_->window,enabled))return false;return enabled?SDL_HideCursor():SDL_ShowCursor();}
std::size_t VideoPresenter::pending_frame_count()const{return 0;}
std::uint64_t VideoPresenter::presented_frames()const{return impl_?impl_->presented:0;}
std::string VideoPresenter::backend_name()const{const char*driver=SDL_GetCurrentVideoDriver();const std::string base=driver&&*driver?driver:"unconfigured";return impl_&&impl_->renderer?base+"+sdl-renderer="+impl_->renderer_name:base+"+sdl-renderer=unconfigured";}
std::string VideoPresenter::presentation_mode()const{return impl_&&impl_->immediate?"immediate-active":"sdl-managed";}
bool VideoPresenter::is_open()const{return impl_&&impl_->window&&impl_->renderer;}
void VideoPresenter::close(){if(!impl_)return;if(impl_->window){(void)SDL_SetWindowMouseGrab(impl_->window,false);(void)SDL_SetWindowRelativeMouseMode(impl_->window,false);(void)SDL_ShowCursor();}if(impl_->texture){SDL_DestroyTexture(impl_->texture);impl_->texture=nullptr;}if(impl_->transfer_frame)av_frame_free(&impl_->transfer_frame);if(impl_->renderer){SDL_DestroyRenderer(impl_->renderer);impl_->renderer=nullptr;}if(impl_->window){SDL_DestroyWindow(impl_->window);impl_->window=nullptr;}}
VideoPresenter::~VideoPresenter(){close();}
}
