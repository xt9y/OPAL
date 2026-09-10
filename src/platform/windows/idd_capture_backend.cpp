#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>

#include <opal/capture_backend.hpp>
#include <opal/windows_idd_capture.hpp>

#include "../../../platform/windows/idd/Protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace opal {
namespace {

std::uint64_t monotonic_us()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

template <class T>
void release_com(T*& value)
{
    if (value) value->Release();
    value = nullptr;
}

class WindowsIddCaptureBackend final : public CaptureBackend {
public:
    ~WindowsIddCaptureBackend() override { stop(); }

    bool start(const StreamOptions& stream) override
    {
        return start(stream, nullptr);
    }

    bool start(const StreamOptions&, const DisplayTarget*) override
    {
        stop();
        error_ = {};

        mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, idd::kFrameMappingName);
        if (!mapping_) {
            set_error("OPAL indirect display frame mapping is unavailable (Win32 " +
                      std::to_string(GetLastError()) + ")");
            return false;
        }
        view_ = static_cast<const std::uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, idd::kFrameMappingBytes));
        if (!view_) {
            set_error("could not map OPAL indirect display frame buffer (Win32 " +
                      std::to_string(GetLastError()) + ")");
            stop_resources();
            return false;
        }
        frame_event_ = OpenEventW(SYNCHRONIZE, FALSE, idd::kFrameEventName);
        if (!frame_event_) {
            set_error("OPAL indirect display frame event is unavailable (Win32 " +
                      std::to_string(GetLastError()) + ")");
            stop_resources();
            return false;
        }

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        D3D_FEATURE_LEVEL selected{};
        constexpr D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
        };
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                       levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                       &device_, &selected, &context_);
        if (hr == E_INVALIDARG) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels + 1, static_cast<UINT>(std::size(levels) - 1), D3D11_SDK_VERSION,
                                   &device_, &selected, &context_);
        }
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                                   levels + 1, static_cast<UINT>(std::size(levels) - 1), D3D11_SDK_VERSION,
                                   &device_, &selected, &context_);
        }
        if (FAILED(hr) || !device_ || !context_) {
            set_error("could not create D3D11 upload device for OPAL indirect display");
            stop_resources();
            return false;
        }

        running_ = true;
        return true;
    }

    bool next(NativeVideoFrame& frame, int timeout_ms) override
    {
        frame = {};
        if (!running_ || !view_ || !frame_event_) return false;
        const DWORD wait = WaitForSingleObject(frame_event_, timeout_ms <= 0 ? 0 : static_cast<DWORD>(timeout_ms));
        if (wait == WAIT_TIMEOUT) return false;
        if (wait != WAIT_OBJECT_0) {
            set_error("waiting for OPAL indirect display frame failed (Win32 " +
                      std::to_string(GetLastError()) + ")");
            running_ = false;
            return false;
        }

        const auto* shared = reinterpret_cast<const idd::SharedFrameHeader*>(view_);
        if (shared->magic != idd::kFrameMagic || shared->version != idd::kProtocolVersion) return false;

        for (int attempt = 0; attempt < 3; ++attempt) {
            auto* sequence_ptr = reinterpret_cast<volatile LONG64*>(const_cast<std::int64_t*>(&shared->sequence));
            const LONG64 before = InterlockedCompareExchange64(sequence_ptr, 0, 0);
            if ((before & 1) != 0) {
                SwitchToThread();
                continue;
            }
            MemoryBarrier();

            const std::uint32_t width = shared->width;
            const std::uint32_t height = shared->height;
            const std::uint32_t stride = shared->stride;
            const std::uint32_t bytes = shared->bytes;
            if (width == 0 || height == 0 || width > idd::kMaxWidth || height > idd::kMaxHeight ||
                stride < width * idd::kBytesPerPixel || bytes == 0 || bytes > idd::kMaxFrameBytes ||
                static_cast<std::uint64_t>(stride) * height > bytes) {
                set_error("OPAL indirect display published an invalid frame layout");
                running_ = false;
                return false;
            }

            pixels_.resize(bytes);
            std::memcpy(pixels_.data(), view_ + sizeof(idd::SharedFrameHeader), bytes);
            MemoryBarrier();
            const LONG64 after = InterlockedCompareExchange64(sequence_ptr, 0, 0);
            if (before != after || (after & 1) != 0) continue;

            if (!ensure_texture(width, height)) return false;
            context_->UpdateSubresource(texture_, 0, nullptr, pixels_.data(), stride, 0);

            texture_->AddRef();
            frame.kind = NativeVideoFrameKind::Opaque;
            frame.width = static_cast<int>(width);
            frame.height = static_cast<int>(height);
            frame.pixel_format = static_cast<std::uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM);
            frame.capture_time_us = monotonic_us();
            frame.opaque = texture_;
            frame.owner = std::shared_ptr<void>(texture_, [](void* value) {
                static_cast<ID3D11Texture2D*>(value)->Release();
            });
            return true;
        }
        return false;
    }

    void stop() override
    {
        running_ = false;
        pixels_.clear();
        release_com(texture_);
        release_com(context_);
        release_com(device_);
        stop_resources();
        width_ = height_ = 0;
    }

    CaptureTimestampQuality timestamp_quality() const override
    {
        // The driver publishes the DWM swapchain promptly, but this first
        // implementation crosses a CPU shared-memory handoff before OPAL's
        // monotonic clock is sampled.
        return CaptureTimestampQuality::Estimated;
    }

    std::string backend_name() const override
    {
        return "iddcx-shared-frame+d3d11-upload";
    }

    PlatformError last_platform_error() const override { return error_; }

private:
    bool ensure_texture(std::uint32_t width, std::uint32_t height)
    {
        if (texture_ && width_ == width && height_ == height) return true;
        release_com(texture_);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        const HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &texture_);
        if (FAILED(hr) || !texture_) {
            set_error("could not create D3D11 texture for OPAL indirect display frame");
            running_ = false;
            return false;
        }
        width_ = width;
        height_ = height;
        return true;
    }

    void stop_resources()
    {
        if (view_) {
            UnmapViewOfFile(view_);
            view_ = nullptr;
        }
        if (mapping_) {
            CloseHandle(mapping_);
            mapping_ = nullptr;
        }
        if (frame_event_) {
            CloseHandle(frame_event_);
            frame_event_ = nullptr;
        }
    }

    void set_error(std::string message)
    {
        error_ = {PlatformComponent::Capture, PlatformFailure::OsError, std::move(message), true};
    }

    bool running_ = false;
    HANDLE mapping_ = nullptr;
    HANDLE frame_event_ = nullptr;
    const std::uint8_t* view_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11Texture2D* texture_ = nullptr;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::vector<std::uint8_t> pixels_;
    PlatformError error_{};
};

}

std::unique_ptr<CaptureBackend> make_windows_idd_capture_backend()
{
    return std::make_unique<WindowsIddCaptureBackend>();
}

}
