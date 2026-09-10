#pragma once

#define NOMINMAX
#include <windows.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <iddcx.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <avrt.h>
#include <wrl.h>

#include "Protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace opal::idd::driver {

class FrameStore {
public:
    bool publish(ID3D11Texture2D* surface, ID3D11Device* device, ID3D11DeviceContext* context);
    NTSTATUS copy_if_new(std::int64_t last_sequence, void* output, std::size_t output_bytes,
                         std::size_t& written);

private:
    std::mutex mu_;
    SharedFrameHeader header_{};
    std::vector<std::uint8_t> pixels_;
    std::vector<std::uint8_t> scratch_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    std::uint32_t staging_width_ = 0;
    std::uint32_t staging_height_ = 0;
    DXGI_FORMAT staging_format_ = DXGI_FORMAT_UNKNOWN;
};

class SwapChainProcessor {
public:
    SwapChainProcessor(IDDCX_SWAPCHAIN swapchain, LUID render_adapter,
                       HANDLE available_event, std::shared_ptr<FrameStore> frames);
    ~SwapChainProcessor();

private:
    void run();

    IDDCX_SWAPCHAIN swapchain_ = nullptr;
    LUID render_adapter_{};
    HANDLE available_event_ = nullptr;
    HANDLE stop_event_ = nullptr;
    std::shared_ptr<FrameStore> frames_;
    std::thread thread_;
};

class MonitorContext {
public:
    MonitorContext(IDDCX_MONITOR monitor, DisplayModeRequest mode,
                   std::shared_ptr<FrameStore> frames);
    ~MonitorContext();

    void assign(IDDCX_SWAPCHAIN swapchain, LUID render_adapter, HANDLE available_event);
    void unassign();
    const DisplayModeRequest& mode() const noexcept { return mode_; }

private:
    IDDCX_MONITOR monitor_ = nullptr;
    DisplayModeRequest mode_{};
    std::shared_ptr<FrameStore> frames_;
    std::unique_ptr<SwapChainProcessor> processor_;
};

class DeviceContext {
public:
    explicit DeviceContext(WDFDEVICE device);
    ~DeviceContext();

    void initialize_adapter();
    void adapter_initialized(NTSTATUS status);
    NTSTATUS create_monitor(const DisplayModeRequest& mode);
    NTSTATUS set_mode(const DisplayModeRequest& mode);
    NTSTATUS destroy_monitor();
    DriverStatus status();
    std::shared_ptr<FrameStore> frames() const { return frames_; }

private:
    NTSTATUS create_monitor_locked(const DisplayModeRequest& mode);
    NTSTATUS destroy_monitor_locked();

    WDFDEVICE device_ = nullptr;
    IDDCX_ADAPTER adapter_ = nullptr;
    IDDCX_MONITOR monitor_ = nullptr;
    DisplayModeRequest mode_{};
    bool adapter_ready_ = false;
    std::shared_ptr<FrameStore> frames_;
    std::mutex mu_;
};

struct DeviceContextWrapper { DeviceContext* value = nullptr; };
struct MonitorContextWrapper { MonitorContext* value = nullptr; };

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContextWrapper, OpalGetDeviceContext);
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MonitorContextWrapper, OpalGetMonitorContext);

}
