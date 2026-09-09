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

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>

namespace opal {
namespace {
using Clock = std::chrono::steady_clock;

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

struct AcquiredDesktopFrame {
    IDXGIOutputDuplication* duplication = nullptr;
    ID3D11Texture2D* texture = nullptr;

    ~AcquiredDesktopFrame()
    {
        if (texture) texture->Release();
        if (duplication) {
            (void)duplication->ReleaseFrame();
            duplication->Release();
        }
    }
};

class WindowsCaptureBackend final : public CaptureBackend {
public:
    ~WindowsCaptureBackend() override { stop(); }

    bool start(const StreamOptions& stream) override
    {
        stop();
        stream_ = stream;
        error_ = {};
        timestamp_quality_ = CaptureTimestampQuality::Estimated;

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
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
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                       levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                       &device_, &selected, &context_);
        if (hr == E_INVALIDARG) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels + 1, static_cast<UINT>(std::size(levels) - 1), D3D11_SDK_VERSION,
                                   &device_, &selected, &context_);
        }
        if (FAILED(hr) || !device_ || !context_) {
            error_ = dxgi_error(hr, "D3D11 hardware device creation failed");
            stop_device();
            return false;
        }
        if (!open_duplication()) {
            stop_device();
            return false;
        }
        running_ = true;
        return true;
    }

    bool next(NativeVideoFrame& frame, int timeout_ms) override
    {
        frame = {};
        if (!running_ || !duplication_) return false;
        const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(0, timeout_ms));

        for (;;) {
            const auto now = Clock::now();
            const int remaining = timeout_ms <= 0 ? 0 : static_cast<int>(std::max<std::int64_t>(
                0, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count()));

            DXGI_OUTDUPL_FRAME_INFO info{};
            IDXGIResource* resource = nullptr;
            const HRESULT hr = duplication_->AcquireNextFrame(static_cast<UINT>(remaining), &info, &resource);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
            if (hr == DXGI_ERROR_ACCESS_LOST) {
                if (!open_duplication()) {
                    running_ = false;
                    return false;
                }
                if (timeout_ms <= 0 || Clock::now() >= deadline) return false;
                continue;
            }
            if (FAILED(hr) || !resource) {
                error_ = dxgi_error(hr, "Desktop Duplication AcquireNextFrame failed", true);
                running_ = false;
                if (resource) resource->Release();
                return false;
            }

            ID3D11Texture2D* texture = nullptr;
            const HRESULT texture_hr = resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                                                 reinterpret_cast<void**>(&texture));
            resource->Release();
            if (FAILED(texture_hr) || !texture) {
                (void)duplication_->ReleaseFrame();
                error_ = dxgi_error(texture_hr, "Desktop Duplication frame is not a D3D11 texture", true);
                running_ = false;
                return false;
            }

            if (info.LastPresentTime.QuadPart == 0) {
                texture->Release();
                (void)duplication_->ReleaseFrame();
                if (timeout_ms <= 0 || Clock::now() >= deadline) return false;
                continue;
            }

            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            if (desc.Width == 0 || desc.Height == 0) {
                texture->Release();
                (void)duplication_->ReleaseFrame();
                return false;
            }

            const std::uint64_t callback_us = monotonic_us();
            CaptureTimestampQuality quality = CaptureTimestampQuality::Estimated;
            const std::uint64_t capture_us = present_time_us(info.LastPresentTime, callback_us, quality);

            duplication_->AddRef();
            auto owner = std::make_shared<AcquiredDesktopFrame>();
            owner->duplication = duplication_;
            owner->texture = texture;

            frame.kind = NativeVideoFrameKind::Opaque;
            frame.width = static_cast<int>(desc.Width);
            frame.height = static_cast<int>(desc.Height);
            frame.stride = 0;
            frame.pixel_format = static_cast<std::uint32_t>(desc.Format);
            frame.capture_time_us = capture_us;
            frame.opaque = texture;
            frame.owner = std::static_pointer_cast<void>(owner);
            timestamp_quality_ = quality;
            return true;
        }
    }

    void stop() override
    {
        running_ = false;
        if (duplication_) {
            duplication_->Release();
            duplication_ = nullptr;
        }
        stop_device();
        timestamp_quality_ = CaptureTimestampQuality::Estimated;
    }

    CaptureTimestampQuality timestamp_quality() const override { return timestamp_quality_; }
    std::string backend_name() const override { return "dxgi-desktop-duplication+d3d11"; }
    PlatformError last_platform_error() const override { return error_; }

private:
    bool open_duplication()
    {
        if (!device_) return false;
        if (duplication_) {
            duplication_->Release();
            duplication_ = nullptr;
        }

        IDXGIDevice* dxgi_device = nullptr;
        HRESULT hr = device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device));
        if (FAILED(hr) || !dxgi_device) {
            error_ = dxgi_error(hr, "D3D11 device does not expose IDXGIDevice");
            return false;
        }

        IDXGIAdapter* adapter = nullptr;
        hr = dxgi_device->GetAdapter(&adapter);
        dxgi_device->Release();
        if (FAILED(hr) || !adapter) {
            error_ = dxgi_error(hr, "could not resolve DXGI adapter");
            return false;
        }

        IDXGIOutput* output = nullptr;
        hr = adapter->EnumOutputs(0, &output);
        adapter->Release();
        if (FAILED(hr) || !output) {
            error_ = dxgi_error(hr, "no capturable DXGI output is available");
            return false;
        }

        IDXGIOutput1* output1 = nullptr;
        hr = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
        output->Release();
        if (FAILED(hr) || !output1) {
            error_ = dxgi_error(hr, "DXGI output does not support Desktop Duplication");
            return false;
        }

        hr = output1->DuplicateOutput(device_, &duplication_);
        output1->Release();
        if (FAILED(hr) || !duplication_) {
            error_ = dxgi_error(hr, "IDXGIOutput1::DuplicateOutput failed", true);
            return false;
        }
        error_ = {};
        return true;
    }

    void stop_device()
    {
        if (context_) {
            context_->ClearState();
            context_->Flush();
            context_->Release();
            context_ = nullptr;
        }
        if (device_) {
            device_->Release();
            device_ = nullptr;
        }
    }

    StreamOptions stream_{};
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGIOutputDuplication* duplication_ = nullptr;
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
