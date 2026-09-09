#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <opal/capture_backend.hpp>

#include "cursor_compositor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace opal {
namespace {
using Clock = std::chrono::steady_clock;
using windows_detail::CursorUpdate;

std::uint64_t monotonic_us()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now().time_since_epoch()).count());
}

std::string hresult_text(HRESULT value)
{
    std::ostringstream out;
    out << "HRESULT 0x" << std::hex << static_cast<unsigned long>(value);
    return out.str();
}

PlatformError dxgi_error(HRESULT value, std::string message, bool fallback = false)
{
    PlatformError error;
    error.component = PlatformComponent::Capture;
    error.failure = value == DXGI_ERROR_UNSUPPORTED ? PlatformFailure::Unsupported : PlatformFailure::OsError;
    error.message = std::move(message) + " (" + hresult_text(value) + ")";
    error.fallback_possible = fallback;
    return error;
}

std::uint64_t present_time_us(const LARGE_INTEGER& present_time, std::uint64_t callback_us,
                              CaptureTimestampQuality& quality)
{
    quality = CaptureTimestampQuality::Estimated;
    if (present_time.QuadPart <= 0) return callback_us;

    LARGE_INTEGER now{};
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceCounter(&now) || !QueryPerformanceFrequency(&frequency) ||
        frequency.QuadPart <= 0 || now.QuadPart < present_time.QuadPart) return callback_us;

    const auto delta = static_cast<long double>(now.QuadPart - present_time.QuadPart);
    const auto age_us_value = delta * 1000000.0L / static_cast<long double>(frequency.QuadPart);
    if (age_us_value < 0.0L || age_us_value > static_cast<long double>(callback_us)) return callback_us;
    quality = CaptureTimestampQuality::Exact;
    return callback_us - static_cast<std::uint64_t>(age_us_value);
}

template <class T>
void release_com(T*& value)
{
    if (value) value->Release();
    value = nullptr;
}

bool same_rect(const RECT& a, const RECT& b)
{
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

bool rotation_value(DXGI_MODE_ROTATION dxgi, D3D11_VIDEO_PROCESSOR_ROTATION& d3d)
{
    switch (dxgi) {
        case DXGI_MODE_ROTATION_UNSPECIFIED:
        case DXGI_MODE_ROTATION_IDENTITY:
            d3d = D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY;
            return true;
        case DXGI_MODE_ROTATION_ROTATE90:
            d3d = D3D11_VIDEO_PROCESSOR_ROTATION_90;
            return true;
        case DXGI_MODE_ROTATION_ROTATE180:
            d3d = D3D11_VIDEO_PROCESSOR_ROTATION_180;
            return true;
        case DXGI_MODE_ROTATION_ROTATE270:
            d3d = D3D11_VIDEO_PROCESSOR_ROTATION_270;
            return true;
        default:
            return false;
    }
}

struct AcquiredDesktopFrame {
    IDXGIOutputDuplication* duplication = nullptr;
    ID3D11Texture2D* texture = nullptr;

    ~AcquiredDesktopFrame()
    {
        release_com(texture);
        if (duplication) {
            (void)duplication->ReleaseFrame();
            duplication->Release();
            duplication = nullptr;
        }
    }
};

struct VirtualDesktopLayout {
    RECT bounds{};
    int source_width = 0;
    int source_height = 0;
    int canvas_width = 0;
    int canvas_height = 0;

    bool valid() const noexcept
    {
        return source_width > 0 && source_height > 0 && canvas_width > 0 && canvas_height > 0;
    }
};

struct OutputCapture {
    IDXGIOutput1* output = nullptr;
    IDXGIOutputDuplication* duplication = nullptr;
    ID3D11Texture2D* latest_texture = nullptr;
    ID3D11VideoProcessorEnumerator* video_enumerator = nullptr;
    ID3D11VideoProcessor* video_processor = nullptr;
    ID3D11VideoProcessorInputView* input_view = nullptr;
    ID3D11VideoProcessorOutputView* output_view = nullptr;
    DXGI_OUTPUT_DESC desc{};
    RECT destination{};
    int frame_width = 0;
    int frame_height = 0;
    DXGI_FORMAT frame_format = DXGI_FORMAT_UNKNOWN;
    std::uint64_t capture_us = 0;
    CaptureTimestampQuality timestamp_quality = CaptureTimestampQuality::Estimated;
    bool ready = false;
};

enum class AcquireResult { None, Updated, PointerUpdated, Fatal };

class WindowsCaptureBackend final : public CaptureBackend {
public:
    ~WindowsCaptureBackend() override { stop(); }

    bool start(const StreamOptions& stream) override
    {
        stop();
        stream_ = stream;
        error_ = {};
        timestamp_quality_ = CaptureTimestampQuality::Estimated;

        if (!select_adapter()) return fail_start();
        if (selected_attached_outputs_ != total_attached_outputs_) {
            error_ = {PlatformComponent::Capture, PlatformFailure::Unsupported,
                      "desktop spans multiple DXGI adapters; cross-adapter capture is not supported", false};
            return fail_start();
        }
        if (!create_device()) return fail_start();
        if (!open_outputs()) return fail_start();
        if (!build_virtual_desktop()) return fail_start();

        needs_composite_ = outputs_.size() > 1;
        for (const auto& output : outputs_)
            needs_composite_ |= output.desc.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
                                output.desc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED;
        if (needs_composite_ && !ensure_composite_surface()) return fail_start();

        running_ = true;
        return true;
    }

    bool next(NativeVideoFrame& frame, int timeout_ms) override
    {
        frame = {};
        if (!running_ || outputs_.empty()) return false;
        return needs_composite_ ? next_composite(frame, timeout_ms) : next_single(frame, timeout_ms);
    }

    void stop() override
    {
        running_ = false;
        cursor_.reset();
        release_com(composite_rtv_);
        release_com(composite_texture_);
        for (auto& output : outputs_) release_output(output);
        outputs_.clear();
        release_com(video_context_);
        release_com(video_device_);
        if (context_) {
            context_->ClearState();
            context_->Flush();
        }
        release_com(context_);
        release_com(device_);
        release_com(adapter_);
        virtual_desktop_ = {};
        selected_attached_outputs_ = 0;
        total_attached_outputs_ = 0;
        needs_composite_ = false;
        timestamp_quality_ = CaptureTimestampQuality::Estimated;
    }

    CaptureTimestampQuality timestamp_quality() const override { return timestamp_quality_; }

    std::string backend_name() const override
    {
        std::string name = "dxgi-desktop-duplication+d3d11";
        if (outputs_.size() > 1) name += "+multimonitor-gpu";
        if (needs_composite_ && outputs_.size() == 1) name += "+gpu-composite";
        if (cursor_.visible()) name += "+cursor-shader";
        return name;
    }

    PlatformError last_platform_error() const override { return error_; }

private:
    bool fail_start()
    {
        PlatformError failure = error_;
        stop();
        error_ = std::move(failure);
        return false;
    }

    bool select_adapter()
    {
        IDXGIFactory1* factory = nullptr;
        HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
        if (FAILED(hr) || !factory) {
            error_ = dxgi_error(hr, "DXGI factory creation failed");
            return false;
        }

        IDXGIAdapter1* best = nullptr;
        unsigned best_outputs = 0;
        bool best_primary = false;
        total_attached_outputs_ = 0;

        for (UINT adapter_index = 0;; ++adapter_index) {
            IDXGIAdapter1* candidate = nullptr;
            hr = factory->EnumAdapters1(adapter_index, &candidate);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr) || !candidate) continue;

            DXGI_ADAPTER_DESC1 adapter_desc{};
            candidate->GetDesc1(&adapter_desc);
            if ((adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                candidate->Release();
                continue;
            }

            unsigned attached = 0;
            bool primary = false;
            for (UINT output_index = 0;; ++output_index) {
                IDXGIOutput* output = nullptr;
                const HRESULT output_hr = candidate->EnumOutputs(output_index, &output);
                if (output_hr == DXGI_ERROR_NOT_FOUND) break;
                if (FAILED(output_hr) || !output) continue;
                DXGI_OUTPUT_DESC desc{};
                if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop) {
                    ++attached;
                    ++total_attached_outputs_;
                    const POINT origin{0, 0};
                    primary |= desc.Monitor == MonitorFromPoint(origin, MONITOR_DEFAULTTONULL);
                }
                output->Release();
            }

            const bool better = attached > best_outputs ||
                                (attached == best_outputs && primary && !best_primary);
            if (attached > 0 && better) {
                release_com(best);
                best = candidate;
                best_outputs = attached;
                best_primary = primary;
            } else {
                candidate->Release();
            }
        }
        factory->Release();

        if (!best) {
            error_ = {PlatformComponent::Capture, PlatformFailure::Unavailable,
                      "no hardware DXGI adapter owns an attached desktop output", false};
            return false;
        }
        adapter_ = best;
        selected_attached_outputs_ = best_outputs;
        return true;
    }

    bool create_device()
    {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
        if (const char* debug = std::getenv("OPAL_D3D_DEBUG"); debug && *debug && std::string(debug) != "0")
            flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        constexpr D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };
        D3D_FEATURE_LEVEL selected{};
        HRESULT hr = D3D11CreateDevice(adapter_, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                                       levels, static_cast<UINT>(sizeof(levels) / sizeof(levels[0])),
                                       D3D11_SDK_VERSION, &device_, &selected, &context_);
        if (hr == E_INVALIDARG) {
            hr = D3D11CreateDevice(adapter_, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                                   levels + 1, static_cast<UINT>(sizeof(levels) / sizeof(levels[0]) - 1),
                                   D3D11_SDK_VERSION, &device_, &selected, &context_);
        }
        if (FAILED(hr) || !device_ || !context_) {
            error_ = dxgi_error(hr, "D3D11 hardware/video device creation failed");
            return false;
        }

        if (FAILED(device_->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(&video_device_))) ||
            FAILED(context_->QueryInterface(__uuidof(ID3D11VideoContext), reinterpret_cast<void**>(&video_context_))) ||
            !video_device_ || !video_context_) {
            error_ = {PlatformComponent::Capture, PlatformFailure::Unsupported,
                      "D3D11 video processor interfaces are unavailable", false};
            return false;
        }
        return true;
    }

    bool open_outputs()
    {
        if (!adapter_ || !device_) return false;
        outputs_.clear();
        for (UINT output_index = 0;; ++output_index) {
            IDXGIOutput* base = nullptr;
            const HRESULT enum_hr = adapter_->EnumOutputs(output_index, &base);
            if (enum_hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(enum_hr) || !base) continue;

            DXGI_OUTPUT_DESC desc{};
            const HRESULT desc_hr = base->GetDesc(&desc);
            if (FAILED(desc_hr) || !desc.AttachedToDesktop) {
                base->Release();
                continue;
            }

            IDXGIOutput1* output1 = nullptr;
            const HRESULT query_hr = base->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
            base->Release();
            if (FAILED(query_hr) || !output1) {
                error_ = dxgi_error(query_hr, "attached DXGI output does not support Desktop Duplication");
                return false;
            }

            OutputCapture output{};
            output.output = output1;
            output.desc = desc;
            if (!open_duplication(output)) {
                release_output(output);
                return false;
            }
            outputs_.push_back(output);
        }
        if (outputs_.empty()) {
            error_ = {PlatformComponent::Capture, PlatformFailure::Unavailable,
                      "selected DXGI adapter has no capturable desktop outputs", false};
            return false;
        }
        return true;
    }

    bool build_virtual_desktop()
    {
        LONG min_x = std::numeric_limits<LONG>::max();
        LONG min_y = std::numeric_limits<LONG>::max();
        LONG max_x = std::numeric_limits<LONG>::min();
        LONG max_y = std::numeric_limits<LONG>::min();
        for (const auto& output : outputs_) {
            min_x = std::min(min_x, output.desc.DesktopCoordinates.left);
            min_y = std::min(min_y, output.desc.DesktopCoordinates.top);
            max_x = std::max(max_x, output.desc.DesktopCoordinates.right);
            max_y = std::max(max_y, output.desc.DesktopCoordinates.bottom);
        }
        const int source_width = static_cast<int>(max_x - min_x);
        const int source_height = static_cast<int>(max_y - min_y);
        if (source_width <= 0 || source_height <= 0) {
            error_ = {PlatformComponent::Capture, PlatformFailure::InvalidState,
                      "DXGI virtual desktop geometry is invalid", false};
            return false;
        }

        const int max_width = stream_.max_width > 0 ? stream_.max_width : source_width;
        const int max_height = stream_.max_height > 0 ? stream_.max_height : source_height;
        const double scale = std::min({1.0,
            static_cast<double>(max_width) / static_cast<double>(source_width),
            static_cast<double>(max_height) / static_cast<double>(source_height)});
        int canvas_width = std::max(2, static_cast<int>(std::floor(source_width * scale)));
        int canvas_height = std::max(2, static_cast<int>(std::floor(source_height * scale)));
        canvas_width &= ~1;
        canvas_height &= ~1;

        virtual_desktop_.bounds = RECT{min_x, min_y, max_x, max_y};
        virtual_desktop_.source_width = source_width;
        virtual_desktop_.source_height = source_height;
        virtual_desktop_.canvas_width = canvas_width;
        virtual_desktop_.canvas_height = canvas_height;
        if (!virtual_desktop_.valid()) return false;

        for (auto& output : outputs_) {
            const auto& source = output.desc.DesktopCoordinates;
            const auto map_x = [&](LONG value) {
                return static_cast<LONG>(std::llround(
                    static_cast<long double>(value - min_x) * canvas_width / source_width));
            };
            const auto map_y = [&](LONG value) {
                return static_cast<LONG>(std::llround(
                    static_cast<long double>(value - min_y) * canvas_height / source_height));
            };
            output.destination = RECT{
                std::clamp<LONG>(map_x(source.left), 0, canvas_width),
                std::clamp<LONG>(map_y(source.top), 0, canvas_height),
                std::clamp<LONG>(map_x(source.right), 0, canvas_width),
                std::clamp<LONG>(map_y(source.bottom), 0, canvas_height)};
            if (output.destination.right <= output.destination.left)
                output.destination.right = std::min<LONG>(canvas_width, output.destination.left + 1);
            if (output.destination.bottom <= output.destination.top)
                output.destination.bottom = std::min<LONG>(canvas_height, output.destination.top + 1);
        }
        return true;
    }

    bool ensure_composite_surface()
    {
        if (composite_texture_ && composite_rtv_) return true;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(virtual_desktop_.canvas_width);
        desc.Height = static_cast<UINT>(virtual_desktop_.canvas_height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &composite_texture_);
        if (SUCCEEDED(hr)) hr = device_->CreateRenderTargetView(composite_texture_, nullptr, &composite_rtv_);
        if (FAILED(hr) || !composite_texture_ || !composite_rtv_) {
            error_ = dxgi_error(hr, "D3D11 desktop composite surface creation failed");
            return false;
        }
        return true;
    }

    bool open_duplication(OutputCapture& output)
    {
        release_com(output.duplication);
        if (!output.output) return false;
        const HRESULT hr = output.output->DuplicateOutput(device_, &output.duplication);
        if (FAILED(hr) || !output.duplication) {
            error_ = dxgi_error(hr, "IDXGIOutput1::DuplicateOutput failed", true);
            return false;
        }
        return true;
    }

    bool refresh_output_after_access_loss(OutputCapture& output)
    {
        if (!output.output) return false;
        DXGI_OUTPUT_DESC next{};
        const HRESULT hr = output.output->GetDesc(&next);
        if (FAILED(hr) || !next.AttachedToDesktop) {
            error_ = dxgi_error(hr, "DXGI output disappeared after access loss", true);
            return false;
        }
        if (!same_rect(next.DesktopCoordinates, output.desc.DesktopCoordinates) || next.Rotation != output.desc.Rotation) {
            error_ = {PlatformComponent::Capture, PlatformFailure::InvalidState,
                      "DXGI desktop topology changed; restart the media generation", true};
            return false;
        }
        output.desc = next;
        return open_duplication(output);
    }

    void release_pipeline(OutputCapture& output)
    {
        release_com(output.output_view);
        release_com(output.input_view);
        release_com(output.video_processor);
        release_com(output.video_enumerator);
    }

    void release_output(OutputCapture& output)
    {
        release_pipeline(output);
        release_com(output.latest_texture);
        release_com(output.duplication);
        release_com(output.output);
        output = {};
    }

    bool ensure_latest_texture(OutputCapture& output, ID3D11Texture2D* source)
    {
        D3D11_TEXTURE2D_DESC source_desc{};
        source->GetDesc(&source_desc);
        if (source_desc.Width == 0 || source_desc.Height == 0 ||
            source_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            error_ = {PlatformComponent::Capture, PlatformFailure::Unsupported,
                      "Desktop Duplication surface is not BGRA8", false};
            return false;
        }

        if (output.latest_texture && output.frame_width == static_cast<int>(source_desc.Width) &&
            output.frame_height == static_cast<int>(source_desc.Height) && output.frame_format == source_desc.Format)
            return true;

        release_pipeline(output);
        release_com(output.latest_texture);

        D3D11_TEXTURE2D_DESC copy = source_desc;
        copy.MipLevels = 1;
        copy.ArraySize = 1;
        copy.Usage = D3D11_USAGE_DEFAULT;
        copy.BindFlags = 0;
        copy.CPUAccessFlags = 0;
        copy.MiscFlags = 0;
        const HRESULT hr = device_->CreateTexture2D(&copy, nullptr, &output.latest_texture);
        if (FAILED(hr) || !output.latest_texture) {
            error_ = dxgi_error(hr, "could not allocate persistent DXGI monitor texture");
            return false;
        }
        output.frame_width = static_cast<int>(source_desc.Width);
        output.frame_height = static_cast<int>(source_desc.Height);
        output.frame_format = source_desc.Format;
        output.ready = false;
        return true;
    }

    bool ensure_output_pipeline(OutputCapture& output)
    {
        if (output.video_processor && output.input_view && output.output_view) return true;
        if (!output.latest_texture || !composite_texture_ || !video_device_ || !video_context_) return false;

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate.Numerator = static_cast<UINT>(std::clamp(stream_.fps, 15, 240));
        content.InputFrameRate.Denominator = 1;
        content.InputWidth = static_cast<UINT>(output.frame_width);
        content.InputHeight = static_cast<UINT>(output.frame_height);
        content.OutputFrameRate = content.InputFrameRate;
        content.OutputWidth = static_cast<UINT>(virtual_desktop_.canvas_width);
        content.OutputHeight = static_cast<UINT>(virtual_desktop_.canvas_height);
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        HRESULT hr = video_device_->CreateVideoProcessorEnumerator(&content, &output.video_enumerator);
        if (SUCCEEDED(hr)) hr = video_device_->CreateVideoProcessor(output.video_enumerator, 0, &output.video_processor);

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
        input_desc.FourCC = 0;
        input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        input_desc.Texture2D.MipSlice = 0;
        input_desc.Texture2D.ArraySlice = 0;
        if (SUCCEEDED(hr)) hr = video_device_->CreateVideoProcessorInputView(
            output.latest_texture, output.video_enumerator, &input_desc, &output.input_view);

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc{};
        output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        output_desc.Texture2D.MipSlice = 0;
        if (SUCCEEDED(hr)) hr = video_device_->CreateVideoProcessorOutputView(
            composite_texture_, output.video_enumerator, &output_desc, &output.output_view);
        if (FAILED(hr) || !output.video_processor || !output.input_view || !output.output_view) {
            release_pipeline(output);
            error_ = dxgi_error(hr, "D3D11 monitor compositor pipeline creation failed");
            return false;
        }

        D3D11_VIDEO_PROCESSOR_ROTATION rotation = D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY;
        if (!rotation_value(output.desc.Rotation, rotation)) {
            error_ = {PlatformComponent::Capture, PlatformFailure::Unsupported,
                      "DXGI output uses an unknown display rotation", false};
            release_pipeline(output);
            return false;
        }
        if (rotation != D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY) {
            D3D11_VIDEO_PROCESSOR_CAPS caps{};
            hr = output.video_enumerator->GetVideoProcessorCaps(&caps);
            if (FAILED(hr) || (caps.FeatureCaps & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ROTATION) == 0) {
                error_ = {PlatformComponent::Capture, PlatformFailure::Unsupported,
                          "D3D11 video processor cannot rotate a captured display", false};
                release_pipeline(output);
                return false;
            }
            video_context_->VideoProcessorSetStreamRotation(output.video_processor, 0, TRUE, rotation);
        }

        RECT source_rect{0, 0, output.frame_width, output.frame_height};
        video_context_->VideoProcessorSetStreamSourceRect(output.video_processor, 0, TRUE, &source_rect);
        video_context_->VideoProcessorSetStreamDestRect(output.video_processor, 0, TRUE, &output.destination);
        video_context_->VideoProcessorSetOutputTargetRect(output.video_processor, TRUE, &output.destination);
        return true;
    }

    CursorUpdate update_cursor(OutputCapture& output, std::size_t output_index,
                               const DXGI_OUTDUPL_FRAME_INFO& info)
    {
        return cursor_.update(output.duplication, output_index, output.desc, info, error_);
    }

    AcquireResult acquire_latest(OutputCapture& output, std::size_t output_index, int timeout_ms)
    {
        if (!output.duplication) return AcquireResult::Fatal;
        DXGI_OUTDUPL_FRAME_INFO info{};
        IDXGIResource* resource = nullptr;
        const HRESULT hr = output.duplication->AcquireNextFrame(static_cast<UINT>(std::max(0, timeout_ms)), &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return AcquireResult::None;
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            if (!refresh_output_after_access_loss(output)) return AcquireResult::Fatal;
            return AcquireResult::None;
        }
        if (FAILED(hr) || !resource) {
            if (resource) resource->Release();
            error_ = dxgi_error(hr, "Desktop Duplication AcquireNextFrame failed", true);
            return AcquireResult::Fatal;
        }

        const CursorUpdate cursor_update = update_cursor(output, output_index, info);
        if (cursor_update == CursorUpdate::Fatal) {
            resource->Release();
            (void)output.duplication->ReleaseFrame();
            return AcquireResult::Fatal;
        }
        if (cursor_.visible() && !needs_composite_) {
            needs_composite_ = true;
            if (!ensure_composite_surface()) {
                resource->Release();
                (void)output.duplication->ReleaseFrame();
                return AcquireResult::Fatal;
            }
        }

        ID3D11Texture2D* texture = nullptr;
        const HRESULT texture_hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
        resource->Release();
        if (FAILED(texture_hr) || !texture) {
            (void)output.duplication->ReleaseFrame();
            error_ = dxgi_error(texture_hr, "Desktop Duplication frame is not a D3D11 texture", true);
            return AcquireResult::Fatal;
        }

        const bool desktop_updated = info.LastPresentTime.QuadPart != 0;
        const bool pointer_updated = cursor_update == CursorUpdate::PointerUpdated;
        if (!desktop_updated && !pointer_updated) {
            texture->Release();
            (void)output.duplication->ReleaseFrame();
            return AcquireResult::None;
        }

        if (!ensure_latest_texture(output, texture)) {
            texture->Release();
            (void)output.duplication->ReleaseFrame();
            return AcquireResult::Fatal;
        }
        context_->CopyResource(output.latest_texture, texture);
        texture->Release();
        (void)output.duplication->ReleaseFrame();
        output.ready = true;

        if (desktop_updated) {
            const std::uint64_t callback_us = monotonic_us();
            output.capture_us = present_time_us(info.LastPresentTime, callback_us, output.timestamp_quality);
            if (pointer_updated && cursor_.capture_time_us() != 0)
                output.capture_us = std::min(output.capture_us, cursor_.capture_time_us());
        } else {
            output.capture_us = cursor_.capture_time_us() ? cursor_.capture_time_us() : monotonic_us();
            output.timestamp_quality = CaptureTimestampQuality::Exact;
        }

        if (needs_composite_ && !ensure_output_pipeline(output)) return AcquireResult::Fatal;
        return desktop_updated ? AcquireResult::Updated : AcquireResult::PointerUpdated;
    }

    bool next_single(NativeVideoFrame& frame, int timeout_ms)
    {
        auto& output = outputs_.front();
        const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(0, timeout_ms));
        for (;;) {
            const auto remaining = timeout_ms <= 0 ? 0 : static_cast<int>(std::max<std::int64_t>(
                0, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count()));
            DXGI_OUTDUPL_FRAME_INFO info{};
            IDXGIResource* resource = nullptr;
            const HRESULT hr = output.duplication->AcquireNextFrame(static_cast<UINT>(remaining), &info, &resource);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
            if (hr == DXGI_ERROR_ACCESS_LOST) {
                if (!refresh_output_after_access_loss(output)) { running_ = false; return false; }
                if (timeout_ms <= 0 || Clock::now() >= deadline) return false;
                continue;
            }
            if (FAILED(hr) || !resource) {
                if (resource) resource->Release();
                error_ = dxgi_error(hr, "Desktop Duplication AcquireNextFrame failed", true);
                running_ = false;
                return false;
            }

            const CursorUpdate cursor_update = update_cursor(output, 0, info);
            if (cursor_update == CursorUpdate::Fatal) {
                resource->Release();
                (void)output.duplication->ReleaseFrame();
                running_ = false;
                return false;
            }

            ID3D11Texture2D* texture = nullptr;
            const HRESULT texture_hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
            resource->Release();
            if (FAILED(texture_hr) || !texture) {
                (void)output.duplication->ReleaseFrame();
                error_ = dxgi_error(texture_hr, "Desktop Duplication frame is not a D3D11 texture", true);
                running_ = false;
                return false;
            }

            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            if (desc.Width == 0 || desc.Height == 0 || desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
                texture->Release();
                (void)output.duplication->ReleaseFrame();
                error_ = {PlatformComponent::Capture, PlatformFailure::Unsupported,
                          "Desktop Duplication surface is not BGRA8", false};
                running_ = false;
                return false;
            }

            const bool desktop_updated = info.LastPresentTime.QuadPart != 0;
            const bool pointer_updated = cursor_update == CursorUpdate::PointerUpdated;
            if (cursor_.visible()) {
                needs_composite_ = true;
                if (!ensure_composite_surface() || !ensure_latest_texture(output, texture)) {
                    texture->Release();
                    (void)output.duplication->ReleaseFrame();
                    running_ = false;
                    return false;
                }
                context_->CopyResource(output.latest_texture, texture);
                texture->Release();
                (void)output.duplication->ReleaseFrame();
                output.ready = true;
                if (desktop_updated) {
                    CaptureTimestampQuality quality = CaptureTimestampQuality::Estimated;
                    output.capture_us = present_time_us(info.LastPresentTime, monotonic_us(), quality);
                    output.timestamp_quality = quality;
                    if (pointer_updated && cursor_.capture_time_us() != 0)
                        output.capture_us = std::min(output.capture_us, cursor_.capture_time_us());
                } else {
                    output.capture_us = cursor_.capture_time_us() ? cursor_.capture_time_us() : monotonic_us();
                    output.timestamp_quality = CaptureTimestampQuality::Exact;
                }
                if (!ensure_output_pipeline(output)) { running_ = false; return false; }
                return compose(frame, output.capture_us, output.timestamp_quality);
            }

            if (!desktop_updated) {
                texture->Release();
                (void)output.duplication->ReleaseFrame();
                if (pointer_updated && needs_composite_ && output.ready)
                    return compose(frame, cursor_.capture_time_us(), CaptureTimestampQuality::Exact);
                if (timeout_ms <= 0 || Clock::now() >= deadline) return false;
                continue;
            }

            const auto callback_us = monotonic_us();
            CaptureTimestampQuality quality = CaptureTimestampQuality::Estimated;
            const auto capture_us = present_time_us(info.LastPresentTime, callback_us, quality);
            output.duplication->AddRef();
            auto owner = std::make_shared<AcquiredDesktopFrame>();
            owner->duplication = output.duplication;
            owner->texture = texture;

            frame.kind = NativeVideoFrameKind::Opaque;
            frame.width = static_cast<int>(desc.Width);
            frame.height = static_cast<int>(desc.Height);
            frame.pixel_format = static_cast<std::uint32_t>(desc.Format);
            frame.capture_time_us = capture_us;
            frame.opaque = texture;
            frame.owner = std::static_pointer_cast<void>(owner);
            timestamp_quality_ = quality;
            return true;
        }
    }

    bool all_outputs_ready() const
    {
        for (const auto& output : outputs_) if (!output.ready) return false;
        return true;
    }

    bool compose(NativeVideoFrame& frame, std::uint64_t capture_us, CaptureTimestampQuality quality)
    {
        if (!ensure_composite_surface()) return false;
        constexpr float black[4] = {0.f, 0.f, 0.f, 1.f};
        context_->ClearRenderTargetView(composite_rtv_, black);

        for (auto& output : outputs_) {
            if (!output.ready || !ensure_output_pipeline(output)) return false;
            D3D11_VIDEO_PROCESSOR_STREAM stream{};
            stream.Enable = TRUE;
            stream.OutputIndex = 0;
            stream.InputFrameOrField = 0;
            stream.pInputSurface = output.input_view;
            const HRESULT hr = video_context_->VideoProcessorBlt(
                output.video_processor, output.output_view, 0, 1, &stream);
            if (FAILED(hr)) {
                error_ = dxgi_error(hr, "D3D11 desktop VideoProcessorBlt failed", true);
                running_ = false;
                return false;
            }
        }

        ID3D11Texture2D* final_texture = composite_texture_;
        if (!cursor_.draw(device_, context_, composite_texture_, virtual_desktop_.bounds,
                          virtual_desktop_.canvas_width, virtual_desktop_.canvas_height,
                          final_texture, error_)) {
            running_ = false;
            return false;
        }

        frame.kind = NativeVideoFrameKind::Opaque;
        frame.width = virtual_desktop_.canvas_width;
        frame.height = virtual_desktop_.canvas_height;
        frame.pixel_format = static_cast<std::uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM);
        frame.capture_time_us = capture_us ? capture_us : monotonic_us();
        frame.opaque = final_texture;
        timestamp_quality_ = quality;
        return true;
    }

    bool next_composite(NativeVideoFrame& frame, int timeout_ms)
    {
        const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(0, timeout_ms));
        bool updated = false;
        std::uint64_t oldest_update = std::numeric_limits<std::uint64_t>::max();
        CaptureTimestampQuality quality = CaptureTimestampQuality::Exact;

        auto note = [&](OutputCapture& output, AcquireResult result) {
            if (result != AcquireResult::Updated && result != AcquireResult::PointerUpdated) return;
            updated = true;
            oldest_update = std::min(oldest_update, output.capture_us);
            if (output.timestamp_quality != CaptureTimestampQuality::Exact)
                quality = CaptureTimestampQuality::Estimated;
        };

        for (std::size_t i = 0; i < outputs_.size(); ++i) {
            auto& output = outputs_[i];
            const auto result = acquire_latest(output, i, 0);
            if (result == AcquireResult::Fatal) { running_ = false; return false; }
            note(output, result);
        }
        if (all_outputs_ready() && updated)
            return compose(frame, oldest_update, quality);
        if (timeout_ms <= 0) return false;

        std::size_t cursor_index = 0;
        while (Clock::now() < deadline) {
            const std::size_t index = cursor_index++ % outputs_.size();
            auto& output = outputs_[index];
            const auto remaining = static_cast<int>(std::max<std::int64_t>(
                1, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count()));
            const auto result = acquire_latest(output, index, std::min(1, remaining));
            if (result == AcquireResult::Fatal) { running_ = false; return false; }
            note(output, result);
            if (result == AcquireResult::Updated || result == AcquireResult::PointerUpdated) {
                for (std::size_t peer_index = 0; peer_index < outputs_.size(); ++peer_index) {
                    if (peer_index == index) continue;
                    auto& peer = outputs_[peer_index];
                    const auto peer_result = acquire_latest(peer, peer_index, 0);
                    if (peer_result == AcquireResult::Fatal) { running_ = false; return false; }
                    note(peer, peer_result);
                }
            }
            if (all_outputs_ready() && updated)
                return compose(frame, oldest_update, quality);
        }
        return false;
    }

    StreamOptions stream_{};
    IDXGIAdapter1* adapter_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11VideoDevice* video_device_ = nullptr;
    ID3D11VideoContext* video_context_ = nullptr;
    std::vector<OutputCapture> outputs_;
    VirtualDesktopLayout virtual_desktop_{};
    ID3D11Texture2D* composite_texture_ = nullptr;
    ID3D11RenderTargetView* composite_rtv_ = nullptr;
    windows_detail::CursorCompositor cursor_{};
    std::size_t selected_attached_outputs_ = 0;
    std::size_t total_attached_outputs_ = 0;
    bool needs_composite_ = false;
    bool running_ = false;
    CaptureTimestampQuality timestamp_quality_ = CaptureTimestampQuality::Estimated;
    PlatformError error_{};
};

}

std::unique_ptr<CaptureBackend> make_capture_backend()
{
    return std::make_unique<WindowsCaptureBackend>();
}

}
