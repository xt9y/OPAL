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
#include <iterator>
#include <memory>
#include <string>
#include <thread>
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

        driver_ = CreateFileW(idd::kDevicePath, GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        if (driver_ == INVALID_HANDLE_VALUE) {
            driver_ = nullptr;
            set_error("OPAL indirect display device is unavailable (Win32 " +
                      std::to_string(GetLastError()) + ")");
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
            stop();
            return false;
        }

        reply_.resize(idd::kFrameReplyBytes);
        running_ = true;
        return true;
    }

    bool next(NativeVideoFrame& frame, int timeout_ms) override
    {
        frame = {};
        if (!running_ || !driver_) return false;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(std::max(0, timeout_ms));

        for (;;) {
            idd::FrameRequest request;
            request.last_sequence = last_sequence_;
            DWORD returned = 0;
            const BOOL ok = DeviceIoControl(driver_, idd::kIoctlGetFrame,
                                            &request, static_cast<DWORD>(sizeof(request)),
                                            reply_.data(), static_cast<DWORD>(reply_.size()),
                                            &returned, nullptr);
            if (ok) {
                if (returned < sizeof(idd::SharedFrameHeader)) {
                    set_error("OPAL indirect display driver returned a truncated frame header");
                    running_ = false;
                    return false;
                }
                const auto* header = reinterpret_cast<const idd::SharedFrameHeader*>(reply_.data());
                if (header->magic != idd::kFrameMagic || header->version != idd::kProtocolVersion ||
                    header->sequence <= last_sequence_ || header->width == 0 || header->height == 0 ||
                    header->width > idd::kMaxWidth || header->height > idd::kMaxHeight ||
                    header->stride < header->width * idd::kBytesPerPixel ||
                    header->bytes == 0 || header->bytes > idd::kMaxFrameBytes ||
                    sizeof(idd::SharedFrameHeader) + static_cast<std::size_t>(header->bytes) > returned) {
                    set_error("OPAL indirect display driver returned an invalid frame");
                    running_ = false;
                    return false;
                }

                if (!ensure_texture(header->width, header->height)) return false;
                const auto* pixels = reply_.data() + sizeof(idd::SharedFrameHeader);
                context_->UpdateSubresource(texture_, 0, nullptr, pixels, header->stride, 0);
                last_sequence_ = header->sequence;

                texture_->AddRef();
                frame.kind = NativeVideoFrameKind::Opaque;
                frame.width = static_cast<int>(header->width);
                frame.height = static_cast<int>(header->height);
                frame.pixel_format = static_cast<std::uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM);
                frame.capture_time_us = monotonic_us();
                frame.opaque = texture_;
                frame.owner = std::shared_ptr<void>(texture_, [](void* value) {
                    static_cast<ID3D11Texture2D*>(value)->Release();
                });
                return true;
            }

            const DWORD win_error = GetLastError();
            if (win_error != ERROR_NO_MORE_ITEMS && win_error != ERROR_RETRY && win_error != ERROR_NOT_READY) {
                set_error("OPAL indirect display frame IOCTL failed (Win32 " +
                          std::to_string(win_error) + ")");
                running_ = false;
                return false;
            }
            if (timeout_ms <= 0 || std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void stop() override
    {
        running_ = false;
        reply_.clear();
        release_com(texture_);
        release_com(context_);
        release_com(device_);
        if (driver_) {
            CloseHandle(driver_);
            driver_ = nullptr;
        }
        width_ = height_ = 0;
        last_sequence_ = 0;
    }

    CaptureTimestampQuality timestamp_quality() const override
    {
        return CaptureTimestampQuality::Estimated;
    }

    std::string backend_name() const override
    {
        return "iddcx-device-frame+d3d11-upload";
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

    void set_error(std::string message)
    {
        error_ = {PlatformComponent::Capture, PlatformFailure::OsError, std::move(message), true};
    }

    bool running_ = false;
    HANDLE driver_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11Texture2D* texture_ = nullptr;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::int64_t last_sequence_ = 0;
    std::vector<std::uint8_t> reply_;
    PlatformError error_{};
};

}

std::unique_ptr<CaptureBackend> make_windows_idd_capture_backend()
{
    return std::make_unique<WindowsIddCaptureBackend>();
}

}
