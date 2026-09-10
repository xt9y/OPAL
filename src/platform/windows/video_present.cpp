#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_5.h>

#include <opal/latency_window.hpp>
#include <opal/video_present.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace opal {
namespace {
using Clock = std::chrono::steady_clock;

bool debug_enabled()
{
    const char* value = std::getenv("OPAL_DEBUG");
    return value && *value && std::string(value) != "0";
}

double elapsed_ms(Clock::time_point begin, Clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

template <class T>
void release_com(T*& value)
{
    if (value) value->Release();
    value = nullptr;
}

RECT fitted_rect(int output_width, int output_height, int source_width, int source_height)
{
    output_width = std::max(1, output_width);
    output_height = std::max(1, output_height);
    source_width = std::max(1, source_width);
    source_height = std::max(1, source_height);
    const double source = static_cast<double>(source_width) / source_height;
    const double output = static_cast<double>(output_width) / output_height;
    int width = output_width;
    int height = output_height;
    if (output > source) width = std::max(1, static_cast<int>(std::lround(output_height * source)));
    else if (output < source) height = std::max(1, static_cast<int>(std::lround(output_width / source)));
    const int left = (output_width - width) / 2;
    const int top = (output_height - height) / 2;
    return RECT{left, top, left + width, top + height};
}

}

struct VideoPresenter::Impl {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    std::string renderer_name = "unconfigured";
    int texture_format = AV_PIX_FMT_NONE;
    int texture_width = 0;
    int texture_height = 0;
    int drawable_width = 0;
    int drawable_height = 0;
    std::uint64_t presented = 0;
    std::uint64_t last_size_refresh = 0;
    bool immediate = false;
    bool expect_d3d11 = false;

    ID3D11Device* d3d_device = nullptr;
    ID3D11DeviceContext* d3d_context = nullptr;
    ID3D11VideoDevice* video_device = nullptr;
    ID3D11VideoContext* video_context = nullptr;
    ID3D11VideoProcessorEnumerator* video_enumerator = nullptr;
    ID3D11VideoProcessor* video_processor = nullptr;
    IDXGISwapChain1* swap_chain = nullptr;
    bool allow_tearing = false;
    int processor_source_width = 0;
    int processor_source_height = 0;
    int processor_output_width = 0;
    int processor_output_height = 0;

    LatencyWindow<128> upload_latency;
    LatencyWindow<128> present_latency;
    Clock::time_point last_debug{};

    bool refresh_drawable_size(bool force = false)
    {
        if (!window) return false;
        if (!force && drawable_width > 0 && drawable_height > 0 && presented - last_size_refresh < 8) return true;
        int width = 0;
        int height = 0;
        if (!SDL_GetWindowSizeInPixels(window, &width, &height) || width <= 0 || height <= 0)
            return drawable_width > 0 && drawable_height > 0;
        const bool changed = width != drawable_width || height != drawable_height;
        drawable_width = width;
        drawable_height = height;
        last_size_refresh = presented;
        if (changed && swap_chain) return resize_swap_chain();
        return true;
    }

    void release_sdl_renderer()
    {
        if (texture) { SDL_DestroyTexture(texture); texture = nullptr; }
        if (renderer) { SDL_DestroyRenderer(renderer); renderer = nullptr; }
        texture_format = AV_PIX_FMT_NONE;
        texture_width = texture_height = 0;
        renderer_name = "unconfigured";
    }

    void release_processor()
    {
        release_com(video_processor);
        release_com(video_enumerator);
        processor_source_width = processor_source_height = 0;
        processor_output_width = processor_output_height = 0;
    }

    void release_d3d()
    {
        release_processor();
        release_com(swap_chain);
        release_com(video_context);
        release_com(video_device);
        if (d3d_context) {
            d3d_context->ClearState();
            d3d_context->Flush();
        }
        release_com(d3d_context);
        release_com(d3d_device);
        allow_tearing = false;
    }

    bool init_renderer()
    {
        if (renderer) return true;
        release_d3d();
        renderer = SDL_CreateRenderer(window, nullptr);
        if (!renderer) return false;
        const char* name = SDL_GetRendererName(renderer);
        renderer_name = name && *name ? name : "unknown";
        (void)SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_DISABLED);
        int vsync = 1;
        immediate = SDL_GetRenderVSync(renderer, &vsync) && vsync == SDL_RENDERER_VSYNC_DISABLED;
        (void)SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        return true;
    }

    bool ensure_texture(int width, int height, int format)
    {
        if (!renderer || width <= 0 || height <= 0) return false;
        if (texture && texture_width == width && texture_height == height && texture_format == format) return true;
        if (texture) { SDL_DestroyTexture(texture); texture = nullptr; }

        SDL_PixelFormat pixel_format = SDL_PIXELFORMAT_UNKNOWN;
        SDL_Colorspace colorspace = SDL_COLORSPACE_BT709_LIMITED;
        if (format == AV_PIX_FMT_YUV420P) pixel_format = SDL_PIXELFORMAT_IYUV;
        else if (format == AV_PIX_FMT_YUVJ420P) {
            pixel_format = SDL_PIXELFORMAT_IYUV;
            colorspace = SDL_COLORSPACE_BT709_FULL;
        } else if (format == AV_PIX_FMT_NV12) pixel_format = SDL_PIXELFORMAT_NV12;
        else return false;

        const SDL_PropertiesID properties = SDL_CreateProperties();
        if (!properties) return false;
        const bool configured =
            SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, static_cast<Sint64>(pixel_format)) &&
            SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, static_cast<Sint64>(SDL_TEXTUREACCESS_STREAMING)) &&
            SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, width) &&
            SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, height) &&
            SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, static_cast<Sint64>(colorspace));
        if (configured) texture = SDL_CreateTextureWithProperties(renderer, properties);
        SDL_DestroyProperties(properties);
        if (!texture) return false;
        (void)SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
        texture_width = width;
        texture_height = height;
        texture_format = format;
        return true;
    }

    bool present_cpu(const AVFrame* frame)
    {
        if (!frame || !VideoPresenter::supports_cpu_upload_format(frame->format)) return false;
        if (!renderer && !init_renderer()) return false;
        if (!ensure_texture(frame->width, frame->height, frame->format) || !refresh_drawable_size()) return false;

        const auto upload_begin = Clock::now();
        bool uploaded = false;
        if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P)
            uploaded = SDL_UpdateYUVTexture(texture, nullptr, frame->data[0], frame->linesize[0],
                                            frame->data[1], frame->linesize[1], frame->data[2], frame->linesize[2]);
        else if (frame->format == AV_PIX_FMT_NV12)
            uploaded = SDL_UpdateNVTexture(texture, nullptr, frame->data[0], frame->linesize[0],
                                           frame->data[1], frame->linesize[1]);
        const auto upload_end = Clock::now();
        if (!uploaded) return false;
        upload_latency.push(elapsed_ms(upload_begin, upload_end));

        const RECT fitted = fitted_rect(drawable_width, drawable_height, frame->width, frame->height);
        const SDL_FRect destination{
            static_cast<float>(fitted.left), static_cast<float>(fitted.top),
            static_cast<float>(fitted.right - fitted.left), static_cast<float>(fitted.bottom - fitted.top)};
        const auto present_begin = Clock::now();
        if (!SDL_RenderClear(renderer) || !SDL_RenderTexture(renderer, texture, nullptr, &destination) ||
            !SDL_RenderPresent(renderer)) return false;
        const auto present_end = Clock::now();
        present_latency.push(elapsed_ms(present_begin, present_end));
        ++presented;
        debug_timing("sdl-cpu");
        return true;
    }

    HWND hwnd() const
    {
        if (!window) return nullptr;
        const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
        if (!properties) return nullptr;
        return static_cast<HWND>(SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    }

    bool resize_swap_chain()
    {
        if (!swap_chain || drawable_width <= 0 || drawable_height <= 0) return true;
        release_processor();
        d3d_context->ClearState();
        const UINT flags = allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        const HRESULT hr = swap_chain->ResizeBuffers(0, static_cast<UINT>(drawable_width),
                                                      static_cast<UINT>(drawable_height),
                                                      DXGI_FORMAT_UNKNOWN, flags);
        return SUCCEEDED(hr);
    }

    bool create_processor(int source_width, int source_height)
    {
        if (!video_device || !video_context || source_width <= 0 || source_height <= 0 ||
            drawable_width <= 0 || drawable_height <= 0) return false;
        if (video_processor && processor_source_width == source_width && processor_source_height == source_height &&
            processor_output_width == drawable_width && processor_output_height == drawable_height) return true;
        release_processor();

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate.Numerator = 60;
        content.InputFrameRate.Denominator = 1;
        content.InputWidth = static_cast<UINT>(source_width);
        content.InputHeight = static_cast<UINT>(source_height);
        content.OutputFrameRate.Numerator = 60;
        content.OutputFrameRate.Denominator = 1;
        content.OutputWidth = static_cast<UINT>(drawable_width);
        content.OutputHeight = static_cast<UINT>(drawable_height);
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        HRESULT hr = video_device->CreateVideoProcessorEnumerator(&content, &video_enumerator);
        if (SUCCEEDED(hr)) hr = video_device->CreateVideoProcessor(video_enumerator, 0, &video_processor);
        if (FAILED(hr) || !video_processor) {
            release_processor();
            return false;
        }
        processor_source_width = source_width;
        processor_source_height = source_height;
        processor_output_width = drawable_width;
        processor_output_height = drawable_height;
        return true;
    }

    bool initialize_d3d(ID3D11Texture2D* input_texture)
    {
        if (!input_texture || !window) return false;
        ID3D11Device* frame_device = nullptr;
        input_texture->GetDevice(&frame_device);
        if (!frame_device) return false;
        if (d3d_device == frame_device && swap_chain && video_device && video_context) {
            frame_device->Release();
            return true;
        }

        release_sdl_renderer();
        release_d3d();
        d3d_device = frame_device;
        d3d_device->GetImmediateContext(&d3d_context);
        if (!d3d_context || FAILED(d3d_device->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(&video_device))) ||
            FAILED(d3d_context->QueryInterface(__uuidof(ID3D11VideoContext), reinterpret_cast<void**>(&video_context)))) {
            release_d3d();
            return false;
        }

        IDXGIDevice* dxgi_device = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory2* factory = nullptr;
        HRESULT hr = d3d_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device));
        if (SUCCEEDED(hr)) hr = dxgi_device->GetAdapter(&adapter);
        if (SUCCEEDED(hr)) hr = adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
        release_com(dxgi_device);
        release_com(adapter);
        if (FAILED(hr) || !factory) {
            release_com(factory);
            release_d3d();
            return false;
        }

        IDXGIFactory5* factory5 = nullptr;
        BOOL tearing = FALSE;
        if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&factory5))) && factory5) {
            if (FAILED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))))
                tearing = FALSE;
        }
        release_com(factory5);
        allow_tearing = tearing == TRUE;

        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = static_cast<UINT>(std::max(1, drawable_width));
        description.Height = static_cast<UINT>(std::max(1, drawable_height));
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        description.Flags = allow_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        hr = factory->CreateSwapChainForHwnd(d3d_device, hwnd(), &description, nullptr, nullptr, &swap_chain);
        if (SUCCEEDED(hr)) (void)factory->MakeWindowAssociation(hwnd(), DXGI_MWA_NO_ALT_ENTER);
        release_com(factory);
        if (FAILED(hr) || !swap_chain) {
            release_d3d();
            return false;
        }

        IDXGIDevice1* dxgi_device1 = nullptr;
        if (SUCCEEDED(d3d_device->QueryInterface(__uuidof(IDXGIDevice1), reinterpret_cast<void**>(&dxgi_device1))) && dxgi_device1)
            (void)dxgi_device1->SetMaximumFrameLatency(1);
        release_com(dxgi_device1);
        immediate = true;
        return true;
    }

    bool present_d3d11(const AVFrame* frame)
    {
        if (!frame || frame->format != AV_PIX_FMT_D3D11 || !frame->data[0]) return false;
        if (!refresh_drawable_size(true)) return false;
        auto* input_texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
        if (!initialize_d3d(input_texture) || !refresh_drawable_size() ||
            !create_processor(frame->width, frame->height)) return false;

        D3D11_TEXTURE2D_DESC input_texture_desc{};
        input_texture->GetDesc(&input_texture_desc);
        if (input_texture_desc.Format != DXGI_FORMAT_NV12 && input_texture_desc.Format != DXGI_FORMAT_420_OPAQUE)
            return false;

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
        input_desc.FourCC = 0;
        input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        input_desc.Texture2D.MipSlice = 0;
        input_desc.Texture2D.ArraySlice = static_cast<UINT>(reinterpret_cast<std::intptr_t>(frame->data[1]));
        ID3D11VideoProcessorInputView* input_view = nullptr;
        HRESULT hr = video_device->CreateVideoProcessorInputView(input_texture, video_enumerator, &input_desc, &input_view);
        if (FAILED(hr) || !input_view) return false;

        ID3D11Texture2D* back_buffer = nullptr;
        ID3D11VideoProcessorOutputView* output_view = nullptr;
        hr = swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer));
        if (SUCCEEDED(hr)) {
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc{};
            output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            output_desc.Texture2D.MipSlice = 0;
            hr = video_device->CreateVideoProcessorOutputView(back_buffer, video_enumerator, &output_desc, &output_view);
        }
        release_com(back_buffer);
        if (FAILED(hr) || !output_view) {
            release_com(input_view);
            return false;
        }

        const RECT source{0, 0, frame->width, frame->height};
        const RECT destination = fitted_rect(drawable_width, drawable_height, frame->width, frame->height);
        const RECT output_rect{0, 0, drawable_width, drawable_height};
        video_context->VideoProcessorSetStreamSourceRect(video_processor, 0, TRUE, &source);
        video_context->VideoProcessorSetStreamDestRect(video_processor, 0, TRUE, &destination);
        video_context->VideoProcessorSetOutputTargetRect(video_processor, TRUE, &output_rect);
        D3D11_VIDEO_COLOR black{};
        video_context->VideoProcessorSetOutputBackgroundColor(video_processor, FALSE, &black);

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.OutputIndex = 0;
        stream.InputFrameOrField = 0;
        stream.pInputSurface = input_view;

        const auto submit_begin = Clock::now();
        hr = video_context->VideoProcessorBlt(video_processor, output_view, 0, 1, &stream);
        release_com(output_view);
        release_com(input_view);
        const auto submit_end = Clock::now();
        if (FAILED(hr)) return false;
        upload_latency.push(elapsed_ms(submit_begin, submit_end));

        const auto present_begin = Clock::now();
        const UINT flags = allow_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
        hr = swap_chain->Present(0, flags);
        const auto present_end = Clock::now();
        if (FAILED(hr) && hr != DXGI_STATUS_OCCLUDED) return false;
        present_latency.push(elapsed_ms(present_begin, present_end));
        ++presented;
        debug_timing("d3d11va-direct");
        return true;
    }

    void debug_timing(const char* path)
    {
        if (!debug_enabled()) return;
        const auto now = Clock::now();
        if (last_debug.time_since_epoch().count() != 0 && now - last_debug < std::chrono::seconds(1)) return;
        last_debug = now;
        const auto upload = upload_latency.snapshot();
        const auto present = present_latency.snapshot();
        std::cerr << "OPAL present upload_submit p50=" << upload.p50 << "ms p95=" << upload.p95
                  << "ms p99=" << upload.p99 << "ms render_present p50=" << present.p50
                  << "ms p95=" << present.p95 << "ms p99=" << present.p99 << "ms path=" << path
                  << " presentation=" << (allow_tearing ? "immediate-tearing" : (immediate ? "immediate" : "sdl-managed"))
                  << " photon_timing=unmeasured\n";
    }
};

VideoPresenter::VideoPresenter() : impl_(std::make_unique<Impl>()) {}

bool VideoPresenter::supports_cpu_upload_format(int format)
{
    return format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_NV12;
}

bool VideoPresenter::open(int source_width, int source_height, bool fullscreen, int source_format)
{
    close();
    if (source_width <= 0 || source_height <= 0) return false;
    impl_ = std::make_unique<Impl>();
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    impl_->window = SDL_CreateWindow("OPAL", source_width, source_height, flags);
    if (!impl_->window) {
        close();
        return false;
    }
    if (!impl_->refresh_drawable_size(true)) {
        close();
        return false;
    }
    impl_->expect_d3d11 = source_format == AV_PIX_FMT_D3D11;
    if (!impl_->expect_d3d11) {
        if (!impl_->init_renderer()) {
            close();
            return false;
        }
        if (source_format >= 0 && !impl_->ensure_texture(source_width, source_height, source_format)) {
            close();
            return false;
        }
    }
    return true;
}

bool VideoPresenter::present_borrowed(DecodedVideoView decoded)
{
    if (!decoded.frame || !impl_ || !impl_->window) return false;
    if (decoded.frame->format == AV_PIX_FMT_D3D11) return impl_->present_d3d11(decoded.frame);
    return impl_->present_cpu(decoded.frame);
}

bool VideoPresenter::present(DecodedVideoFrame decoded)
{
    AVFrame* frame = decoded.frame;
    if (!frame) return false;
    const bool ok = present_borrowed({frame, decoded.pts_us});
    av_frame_free(&frame);
    return ok;
}

std::pair<int, int> VideoPresenter::drawable_size() const
{
    if (!impl_ || !impl_->window) return {0, 0};
    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSizeInPixels(impl_->window, &width, &height)) return {0, 0};
    return {width, height};
}

std::pair<int, int> VideoPresenter::window_size() const
{
    if (!impl_ || !impl_->window) return {0, 0};
    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSize(impl_->window, &width, &height)) return {0, 0};
    return {width, height};
}

bool VideoPresenter::set_mouse_capture(bool enabled)
{
    if (!impl_ || !impl_->window) return false;
    if (!SDL_SetWindowRelativeMouseMode(impl_->window, false)) return false;
    if (!SDL_SetWindowMouseGrab(impl_->window, enabled)) return false;
    if (!SDL_ShowCursor()) return false;
    return true;
}

std::size_t VideoPresenter::pending_frame_count() const { return 0; }
std::uint64_t VideoPresenter::presented_frames() const { return impl_ ? impl_->presented : 0; }

std::string VideoPresenter::backend_name() const
{
    if (!impl_ || !impl_->window) return "unconfigured";
    if (impl_->d3d_device) return "windows+d3d11va-direct";
    const char* driver = SDL_GetCurrentVideoDriver();
    const std::string base = driver && *driver ? driver : "windows";
    return impl_->renderer ? base + "+sdl-renderer=" + impl_->renderer_name : base + "+d3d11-pending";
}

std::string VideoPresenter::presentation_mode() const
{
    if (!impl_) return "unconfigured";
    if (impl_->d3d_device) return impl_->allow_tearing ? "immediate-tearing" : "immediate-d3d11";
    return impl_->immediate ? "immediate-active" : "sdl-managed";
}

bool VideoPresenter::is_open() const
{
    return impl_ && impl_->window && (impl_->expect_d3d11 || impl_->renderer);
}

void VideoPresenter::close()
{
    if (!impl_) return;
    if (impl_->window) {
        (void)SDL_SetWindowMouseGrab(impl_->window, false);
        (void)SDL_SetWindowRelativeMouseMode(impl_->window, false);
        (void)SDL_ShowCursor();
    }
    impl_->release_sdl_renderer();
    impl_->release_d3d();
    if (impl_->window) {
        SDL_DestroyWindow(impl_->window);
        impl_->window = nullptr;
    }
}

VideoPresenter::~VideoPresenter() { close(); }

}
