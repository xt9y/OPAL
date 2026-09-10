#include <opal/video_present.hpp>
#include <opal/latency_window.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace opal {
namespace {
using Clock=std::chrono::steady_clock;
bool debug_enabled(){const char*v=std::getenv("OPAL_DEBUG");return v&&*v&&std::string(v)!="0";}
double elapsed_ms(Clock::time_point begin,Clock::time_point end){return std::chrono::duration<double,std::milli>(end-begin).count();}
SDL_FRect fitted_rect(int ww,int wh,int sw,int sh){ww=std::max(1,ww);wh=std::max(1,wh);sw=std::max(1,sw);sh=std::max(1,sh);const double source=static_cast<double>(sw)/static_cast<double>(sh),window=static_cast<double>(ww)/static_cast<double>(wh);float w=static_cast<float>(ww),h=static_cast<float>(wh);if(window>source)w=static_cast<float>(static_cast<double>(wh)*source);else if(window<source)h=static_cast<float>(static_cast<double>(ww)/source);return SDL_FRect{(static_cast<float>(ww)-w)*0.5f,(static_cast<float>(wh)-h)*0.5f,w,h};}

NSString* shader_source(){
    return @"#include <metal_stdlib>\n"
            "using namespace metal;\n"
            "struct VOut { float4 position [[position]]; float2 uv; };\n"
            "vertex VOut vmain(uint id [[vertex_id]]) {\n"
            "  const float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};\n"
            "  const float2 u[3]={float2(0,1),float2(2,1),float2(0,-1)};\n"
            "  VOut o; o.position=float4(p[id],0,1); o.uv=u[id]; return o;\n"
            "}\n"
            "fragment float4 fmain(VOut in [[stage_in]], texture2d<float> ytex [[texture(0)]], texture2d<float> uvtex [[texture(1)]], constant uint &full_range [[buffer(0)]]) {\n"
            "  constexpr sampler s(coord::normalized,address::clamp_to_edge,filter::linear);\n"
            "  float y=ytex.sample(s,in.uv).r; float2 c=uvtex.sample(s,in.uv).rg-float2(0.5);\n"
            "  if(full_range==0) y=(y-(16.0/255.0))*(255.0/219.0);\n"
            "  float3 rgb=float3(y+1.5748*c.y,y-0.1873*c.x-0.4681*c.y,y+1.8556*c.x);\n"
            "  return float4(saturate(rgb),1.0);\n"
            "}\n";
}
}

struct VideoPresenter::Impl{
    SDL_Window*window=nullptr;
    SDL_Renderer*renderer=nullptr;
    SDL_Texture*texture=nullptr;
    SDL_MetalView metal_view=nullptr;
    CAMetalLayer*layer=nil;
    id<MTLDevice>device=nil;
    id<MTLCommandQueue>queue=nil;
    id<MTLRenderPipelineState>pipeline=nil;
    CVMetalTextureCacheRef texture_cache=nullptr;
    std::string renderer_name="unconfigured";
    int texture_format=AV_PIX_FMT_NONE,texture_width=0,texture_height=0;
    int drawable_width=0,drawable_height=0;
    std::uint64_t presented=0,last_size_refresh=0;
    bool metal=false,immediate=false;
    LatencyWindow<128>upload_latency,present_latency;
    Clock::time_point last_debug{};

    bool refresh_drawable_size(bool force=false){
        if(!window)return false;
        if(!force&&drawable_width>0&&drawable_height>0&&presented-last_size_refresh<8)return true;
        int w=0,h=0;if(!SDL_GetWindowSizeInPixels(window,&w,&h)||w<=0||h<=0)return drawable_width>0&&drawable_height>0;
        drawable_width=w;drawable_height=h;last_size_refresh=presented;
        if(layer)layer.drawableSize=CGSizeMake(static_cast<CGFloat>(w),static_cast<CGFloat>(h));
        return true;
    }

    bool init_metal(){
        metal_view=SDL_Metal_CreateView(window);if(!metal_view)return false;
        layer=(__bridge CAMetalLayer*)SDL_Metal_GetLayer(metal_view);if(!layer)return false;
        device=MTLCreateSystemDefaultDevice();if(!device)return false;
        queue=[device newCommandQueue];if(!queue)return false;
        layer.device=device;layer.pixelFormat=MTLPixelFormatBGRA8Unorm;layer.framebufferOnly=YES;layer.maximumDrawableCount=2;layer.presentsWithTransaction=NO;
        if([layer respondsToSelector:@selector(setDisplaySyncEnabled:)])layer.displaySyncEnabled=NO;
        NSError*error=nil;id<MTLLibrary>library=[device newLibraryWithSource:shader_source() options:nil error:&error];
        if(!library){if(debug_enabled()&&error)std::cerr<<"OPAL Metal shader compile failed: "<<[[error localizedDescription] UTF8String]<<"\n";return false;}
        id<MTLFunction>vertex=[library newFunctionWithName:@"vmain"];id<MTLFunction>fragment=[library newFunctionWithName:@"fmain"];
        if(!vertex||!fragment){[vertex release];[fragment release];[library release];return false;}
        MTLRenderPipelineDescriptor*descriptor=[[MTLRenderPipelineDescriptor alloc]init];descriptor.vertexFunction=vertex;descriptor.fragmentFunction=fragment;descriptor.colorAttachments[0].pixelFormat=layer.pixelFormat;
        pipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        [descriptor release];[vertex release];[fragment release];[library release];
        if(!pipeline)return false;
        if(CVMetalTextureCacheCreate(kCFAllocatorDefault,nullptr,device,nullptr,&texture_cache)!=kCVReturnSuccess||!texture_cache)return false;
        metal=true;immediate=true;renderer_name="metal-cvpixelbuffer";return true;
    }

    bool init_renderer(){renderer=SDL_CreateRenderer(window,nullptr);if(!renderer)return false;const char*name=SDL_GetRendererName(renderer);renderer_name=name&&*name?name:"unknown";(void)SDL_SetRenderVSync(renderer,SDL_RENDERER_VSYNC_DISABLED);int vsync=1;immediate=SDL_GetRenderVSync(renderer,&vsync)&&vsync==SDL_RENDERER_VSYNC_DISABLED;(void)SDL_SetRenderDrawColor(renderer,0,0,0,255);return true;}
    bool ensure_texture(int width,int height,int format){if(!renderer||width<=0||height<=0)return false;if(texture&&texture_width==width&&texture_height==height&&texture_format==format)return true;if(texture){SDL_DestroyTexture(texture);texture=nullptr;}SDL_PixelFormat pixel_format=SDL_PIXELFORMAT_UNKNOWN;SDL_Colorspace colorspace=SDL_COLORSPACE_BT709_LIMITED;if(format==AV_PIX_FMT_YUV420P){pixel_format=SDL_PIXELFORMAT_IYUV;}else if(format==AV_PIX_FMT_YUVJ420P){pixel_format=SDL_PIXELFORMAT_IYUV;colorspace=SDL_COLORSPACE_BT709_FULL;}else if(format==AV_PIX_FMT_NV12){pixel_format=SDL_PIXELFORMAT_NV12;}else return false;const SDL_PropertiesID props=SDL_CreateProperties();if(!props)return false;const bool ok=SDL_SetNumberProperty(props,SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,static_cast<Sint64>(pixel_format))&&SDL_SetNumberProperty(props,SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,static_cast<Sint64>(SDL_TEXTUREACCESS_STREAMING))&&SDL_SetNumberProperty(props,SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER,width)&&SDL_SetNumberProperty(props,SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER,height)&&SDL_SetNumberProperty(props,SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER,static_cast<Sint64>(colorspace));if(ok)texture=SDL_CreateTextureWithProperties(renderer,props);SDL_DestroyProperties(props);if(!texture)return false;(void)SDL_SetTextureScaleMode(texture,SDL_SCALEMODE_LINEAR);texture_width=width;texture_height=height;texture_format=format;return true;}

    bool present_metal(const AVFrame*source){
        if(!source||source->format!=AV_PIX_FMT_VIDEOTOOLBOX||!source->data[3]||!metal||!layer||!queue||!pipeline||!texture_cache)return false;
        @autoreleasepool {
            CVPixelBufferRef pixel=reinterpret_cast<CVPixelBufferRef>(source->data[3]);
            const OSType format=CVPixelBufferGetPixelFormatType(pixel);
            const bool full=format==kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
            if(!full&&format!=kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)return false;
            if(!CVPixelBufferIsPlanar(pixel)||CVPixelBufferGetPlaneCount(pixel)<2)return false;
            const size_t width=CVPixelBufferGetWidthOfPlane(pixel,0),height=CVPixelBufferGetHeightOfPlane(pixel,0);
            if(width==0||height==0||!refresh_drawable_size())return false;
            CVMetalTextureRef yref=nullptr,uvref=nullptr;
            CVReturn rc=CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault,texture_cache,pixel,nullptr,MTLPixelFormatR8Unorm,width,height,0,&yref);
            if(rc!=kCVReturnSuccess||!yref)return false;
            rc=CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault,texture_cache,pixel,nullptr,MTLPixelFormatRG8Unorm,CVPixelBufferGetWidthOfPlane(pixel,1),CVPixelBufferGetHeightOfPlane(pixel,1),1,&uvref);
            if(rc!=kCVReturnSuccess||!uvref){CFRelease(yref);return false;}
            id<MTLTexture>y=CVMetalTextureGetTexture(yref),uv=CVMetalTextureGetTexture(uvref);id<CAMetalDrawable>drawable=[layer nextDrawable];
            if(!y||!uv||!drawable){CFRelease(uvref);CFRelease(yref);return false;}
            const auto begin=Clock::now();
            MTLRenderPassDescriptor*pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=drawable.texture;pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);
            id<MTLCommandBuffer>command=[queue commandBuffer];id<MTLRenderCommandEncoder>encoder=[command renderCommandEncoderWithDescriptor:pass];
            if(!command||!encoder){CFRelease(uvref);CFRelease(yref);return false;}
            const SDL_FRect fit=fitted_rect(drawable_width,drawable_height,static_cast<int>(width),static_cast<int>(height));
            [encoder setViewport:MTLViewport{fit.x,fit.y,fit.w,fit.h,0.0,1.0}];[encoder setRenderPipelineState:pipeline];[encoder setFragmentTexture:y atIndex:0];[encoder setFragmentTexture:uv atIndex:1];
            const std::uint32_t full_range=full?1u:0u;[encoder setFragmentBytes:&full_range length:sizeof(full_range) atIndex:0];[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[encoder endEncoding];[command presentDrawable:drawable];[command commit];
            const auto end=Clock::now();present_latency.push(elapsed_ms(begin,end));upload_latency.push(0.0);++presented;debug_timing();
            CFRelease(uvref);CFRelease(yref);return true;
        }
    }

    bool present_sdl(const AVFrame*f){if(!f||!ensure_texture(f->width,f->height,f->format)||!refresh_drawable_size())return false;const auto upload_begin=Clock::now();bool uploaded=false;if(f->format==AV_PIX_FMT_YUV420P||f->format==AV_PIX_FMT_YUVJ420P)uploaded=SDL_UpdateYUVTexture(texture,nullptr,f->data[0],f->linesize[0],f->data[1],f->linesize[1],f->data[2],f->linesize[2]);else if(f->format==AV_PIX_FMT_NV12)uploaded=SDL_UpdateNVTexture(texture,nullptr,f->data[0],f->linesize[0],f->data[1],f->linesize[1]);const auto upload_end=Clock::now();if(!uploaded)return false;upload_latency.push(elapsed_ms(upload_begin,upload_end));const SDL_FRect dst=fitted_rect(drawable_width,drawable_height,f->width,f->height);const auto present_begin=Clock::now();if(!SDL_RenderClear(renderer)||!SDL_RenderTexture(renderer,texture,nullptr,&dst)||!SDL_RenderPresent(renderer))return false;const auto present_end=Clock::now();present_latency.push(elapsed_ms(present_begin,present_end));++presented;debug_timing();return true;}
    bool present_frame(const AVFrame*source){if(metal)return present_metal(source);return source&&VideoPresenter::supports_cpu_upload_format(source->format)&&present_sdl(source);}
    void debug_timing(){if(!debug_enabled())return;const auto now=Clock::now();if(last_debug.time_since_epoch().count()!=0&&now-last_debug<std::chrono::seconds(1))return;last_debug=now;const auto upload=upload_latency.snapshot(),present=present_latency.snapshot();std::cerr<<"OPAL present upload_submit p50="<<upload.p50<<"ms p95="<<upload.p95<<"ms p99="<<upload.p99<<"ms render_present p50="<<present.p50<<"ms p95="<<present.p95<<"ms p99="<<present.p99<<"ms path="<<(metal?"metal-cvpixelbuffer":"sdl-yuv")<<" presentation="<<(immediate?"immediate-active":"sdl-managed")<<"\n";}
};

VideoPresenter::VideoPresenter():impl_(std::make_unique<Impl>()){}
bool VideoPresenter::supports_cpu_upload_format(int format){return format==AV_PIX_FMT_YUV420P||format==AV_PIX_FMT_YUVJ420P||format==AV_PIX_FMT_NV12;}
bool VideoPresenter::open(int sw,int sh,bool fullscreen,int source_format){close();if(sw<=0||sh<=0)return false;impl_=std::make_unique<Impl>();SDL_WindowFlags flags=SDL_WINDOW_RESIZABLE|SDL_WINDOW_HIGH_PIXEL_DENSITY;if(source_format==AV_PIX_FMT_VIDEOTOOLBOX)flags|=SDL_WINDOW_METAL;if(fullscreen)flags|=SDL_WINDOW_FULLSCREEN;impl_->window=SDL_CreateWindow("OPAL",sw,sh,flags);if(!impl_->window){close();return false;}const bool ok=source_format==AV_PIX_FMT_VIDEOTOOLBOX?impl_->init_metal():impl_->init_renderer();if(!ok){close();return false;}if(!impl_->metal&&source_format>=0&&!impl_->ensure_texture(sw,sh,source_format)){close();return false;}if(!impl_->refresh_drawable_size(true)){close();return false;}return true;}
bool VideoPresenter::present_borrowed(DecodedVideoView decoded){return decoded.frame&&impl_&&impl_->window&&impl_->present_frame(decoded.frame);}
bool VideoPresenter::present(DecodedVideoFrame decoded){AVFrame*f=decoded.frame;if(!f)return false;const bool ok=present_borrowed({f,decoded.pts_us});av_frame_free(&f);return ok;}
std::pair<int,int> VideoPresenter::drawable_size()const{if(!impl_||!impl_->window)return{0,0};if(impl_->drawable_width>0&&impl_->drawable_height>0)return{impl_->drawable_width,impl_->drawable_height};int w=0,h=0;if(!SDL_GetWindowSizeInPixels(impl_->window,&w,&h))return{0,0};return{w,h};}
std::pair<int,int> VideoPresenter::window_size()const{if(!impl_||!impl_->window)return{0,0};int w=0,h=0;if(!SDL_GetWindowSize(impl_->window,&w,&h))return{0,0};return{w,h};}
bool VideoPresenter::set_relative_mouse_mode(bool enabled){if(!impl_||!impl_->window)return false;if(!SDL_SetWindowRelativeMouseMode(impl_->window,enabled))return false;if(!SDL_SetWindowMouseGrab(impl_->window,enabled))return false;return enabled?SDL_HideCursor():SDL_ShowCursor();}
std::size_t VideoPresenter::pending_frame_count()const{return 0;}
std::uint64_t VideoPresenter::presented_frames()const{return impl_?impl_->presented:0;}
std::string VideoPresenter::backend_name()const{const char*driver=SDL_GetCurrentVideoDriver();const std::string base=driver&&*driver?driver:"unconfigured";return impl_?base+"+"+impl_->renderer_name:base+"+unconfigured";}
std::string VideoPresenter::presentation_mode()const{return impl_&&impl_->immediate?"immediate-active":"sdl-managed";}
bool VideoPresenter::is_open()const{return impl_&&impl_->window&&(impl_->metal?impl_->pipeline!=nil:impl_->renderer!=nullptr);}
void VideoPresenter::close(){if(!impl_)return;if(impl_->window){(void)SDL_SetWindowMouseGrab(impl_->window,false);(void)SDL_SetWindowRelativeMouseMode(impl_->window,false);(void)SDL_ShowCursor();}if(impl_->texture){SDL_DestroyTexture(impl_->texture);impl_->texture=nullptr;}if(impl_->texture_cache){CVMetalTextureCacheFlush(impl_->texture_cache,0);CFRelease(impl_->texture_cache);impl_->texture_cache=nullptr;}if(impl_->pipeline){[impl_->pipeline release];impl_->pipeline=nil;}if(impl_->queue){[impl_->queue release];impl_->queue=nil;}if(impl_->device){[impl_->device release];impl_->device=nil;}if(impl_->metal_view){SDL_Metal_DestroyView(impl_->metal_view);impl_->metal_view=nullptr;impl_->layer=nil;}if(impl_->renderer){SDL_DestroyRenderer(impl_->renderer);impl_->renderer=nullptr;}if(impl_->window){SDL_DestroyWindow(impl_->window);impl_->window=nullptr;}}
VideoPresenter::~VideoPresenter(){close();}
}
