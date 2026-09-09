#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <opal/platform_error.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace opal::windows_detail {

template <class T>
inline void cursor_release(T*& value)
{
    if (value) value->Release();
    value = nullptr;
}

enum class CursorUpdate { None, PointerUpdated, Fatal };

class CursorCompositor {
public:
    ~CursorCompositor() { reset(); }

    CursorCompositor(const CursorCompositor&) = delete;
    CursorCompositor& operator=(const CursorCompositor&) = delete;
    CursorCompositor() = default;

    void reset()
    {
        cursor_release(cursor_constants_);
        cursor_release(pixel_shader_);
        cursor_release(vertex_shader_);
        cursor_release(cursor_ops_srv_);
        cursor_release(cursor_ops_texture_);
        cursor_release(cursor_target_rtv_);
        cursor_release(cursor_target_texture_);
        cursor_release(base_srv_);
        cursor_release(base_texture_);
        shape_buffer_.clear();
        ops_.clear();
        shape_info_ = {};
        position_ = {};
        visible_ = false;
        shape_valid_ = false;
        who_updated_ = std::numeric_limits<std::size_t>::max();
        last_timestamp_ = 0;
        capture_time_us_ = 0;
        shape_generation_ = 0;
        uploaded_generation_ = 0;
        shape_width_ = 0;
        shape_height_ = 0;
    }

    CursorUpdate update(IDXGIOutputDuplication* duplication,
                        std::size_t output_index,
                        const DXGI_OUTPUT_DESC& output_desc,
                        const DXGI_OUTDUPL_FRAME_INFO& frame,
                        PlatformError& error)
    {
        if (!duplication) return CursorUpdate::Fatal;
        bool changed = false;

        if (frame.LastMouseUpdateTime.QuadPart != 0) {
            bool update_position = true;
            if (!frame.PointerPosition.Visible && who_updated_ != output_index)
                update_position = false;
            if (frame.PointerPosition.Visible && visible_ && who_updated_ != output_index &&
                last_timestamp_ > frame.LastMouseUpdateTime.QuadPart)
                update_position = false;

            if (update_position) {
                const POINT next{
                    frame.PointerPosition.Position.x + output_desc.DesktopCoordinates.left,
                    frame.PointerPosition.Position.y + output_desc.DesktopCoordinates.top};
                const bool next_visible = frame.PointerPosition.Visible != FALSE;
                changed |= next.x != position_.x || next.y != position_.y || next_visible != visible_;
                position_ = next;
                visible_ = next_visible;
                who_updated_ = output_index;
                last_timestamp_ = frame.LastMouseUpdateTime.QuadPart;
                capture_time_us_ = timestamp_to_local_us(frame.LastMouseUpdateTime);
            }
        }

        if (frame.PointerShapeBufferSize != 0) {
            shape_buffer_.resize(frame.PointerShapeBufferSize);
            UINT required = 0;
            DXGI_OUTDUPL_POINTER_SHAPE_INFO next_info{};
            const HRESULT hr = duplication->GetFramePointerShape(
                frame.PointerShapeBufferSize, shape_buffer_.data(), &required, &next_info);
            if (FAILED(hr)) {
                error = cursor_error(hr, "GetFramePointerShape failed");
                return CursorUpdate::Fatal;
            }
            if (required > shape_buffer_.size()) {
                error = {PlatformComponent::Capture, PlatformFailure::InvalidState,
                         "DXGI pointer shape exceeded its advertised buffer", true};
                return CursorUpdate::Fatal;
            }
            shape_buffer_.resize(required);
            shape_info_ = next_info;
            if (!preprocess_shape(error)) return CursorUpdate::Fatal;
            shape_valid_ = true;
            ++shape_generation_;
            changed = true;
        }

        return changed ? CursorUpdate::PointerUpdated : CursorUpdate::None;
    }

    bool visible() const noexcept { return visible_ && shape_valid_ && !ops_.empty(); }
    std::uint64_t capture_time_us() const noexcept { return capture_time_us_; }

    bool draw(ID3D11Device* device,
              ID3D11DeviceContext* context,
              ID3D11Texture2D* base,
              const RECT& desktop_bounds,
              int canvas_width,
              int canvas_height,
              ID3D11Texture2D*& result,
              PlatformError& error)
    {
        result = base;
        if (!visible()) return true;
        if (!device || !context || !base || canvas_width <= 0 || canvas_height <= 0) return false;

        const int desktop_width = desktop_bounds.right - desktop_bounds.left;
        const int desktop_height = desktop_bounds.bottom - desktop_bounds.top;
        if (desktop_width <= 0 || desktop_height <= 0) return false;

        if (!ensure_base_resources(device, base, error) ||
            !ensure_shaders(device, error) ||
            !ensure_cursor_texture(device, error))
            return false;

        const int left = static_cast<int>(std::llround(
            static_cast<long double>(position_.x - desktop_bounds.left) * canvas_width / desktop_width));
        const int top = static_cast<int>(std::llround(
            static_cast<long double>(position_.y - desktop_bounds.top) * canvas_height / desktop_height));
        const int width = std::max(1, static_cast<int>(std::llround(
            static_cast<long double>(shape_width_) * canvas_width / desktop_width)));
        const int height = std::max(1, static_cast<int>(std::llround(
            static_cast<long double>(shape_height_) * canvas_height / desktop_height)));

        const int clip_left = std::clamp(left, 0, canvas_width);
        const int clip_top = std::clamp(top, 0, canvas_height);
        const int clip_right = std::clamp(left + width, 0, canvas_width);
        const int clip_bottom = std::clamp(top + height, 0, canvas_height);
        if (clip_right <= clip_left || clip_bottom <= clip_top) return true;

        context->CopyResource(cursor_target_texture_, base);

        CursorConstants constants{};
        constants.desktop_width = static_cast<std::uint32_t>(canvas_width);
        constants.desktop_height = static_cast<std::uint32_t>(canvas_height);
        constants.cursor_left = left;
        constants.cursor_top = top;
        constants.cursor_width = static_cast<std::uint32_t>(width);
        constants.cursor_height = static_cast<std::uint32_t>(height);
        constants.shape_width = static_cast<std::uint32_t>(shape_width_);
        constants.shape_height = static_cast<std::uint32_t>(shape_height_);
        context->UpdateSubresource(cursor_constants_, 0, nullptr, &constants, 0, 0);

        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = static_cast<float>(clip_left);
        viewport.TopLeftY = static_cast<float>(clip_top);
        viewport.Width = static_cast<float>(clip_right - clip_left);
        viewport.Height = static_cast<float>(clip_bottom - clip_top);
        viewport.MinDepth = 0.f;
        viewport.MaxDepth = 1.f;
        context->RSSetViewports(1, &viewport);

        ID3D11ShaderResourceView* resources[2] = {base_srv_, cursor_ops_srv_};
        context->OMSetRenderTargets(1, &cursor_target_rtv_, nullptr);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex_shader_, nullptr, 0);
        context->PSSetShader(pixel_shader_, nullptr, 0);
        context->PSSetShaderResources(0, 2, resources);
        context->PSSetConstantBuffers(0, 1, &cursor_constants_);
        context->Draw(3, 0);

        ID3D11ShaderResourceView* null_resources[2] = {nullptr, nullptr};
        context->PSSetShaderResources(0, 2, null_resources);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        result = cursor_target_texture_;
        return true;
    }

private:
    struct CursorOp {
        std::uint32_t color = 0;
        std::uint32_t flags = 0;
    };

    struct CursorConstants {
        std::uint32_t desktop_width = 0;
        std::uint32_t desktop_height = 0;
        std::int32_t cursor_left = 0;
        std::int32_t cursor_top = 0;
        std::uint32_t cursor_width = 0;
        std::uint32_t cursor_height = 0;
        std::uint32_t shape_width = 0;
        std::uint32_t shape_height = 0;
    };

    static PlatformError cursor_error(HRESULT hr, std::string message)
    {
        PlatformError error;
        error.component = PlatformComponent::Capture;
        error.failure = hr == DXGI_ERROR_ACCESS_LOST ? PlatformFailure::InvalidState : PlatformFailure::OsError;
        error.message = std::move(message) + " (HRESULT " + std::to_string(static_cast<unsigned long>(hr)) + ")";
        error.fallback_possible = true;
        return error;
    }

    static std::uint64_t local_now_us()
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    static std::uint64_t timestamp_to_local_us(const LARGE_INTEGER& timestamp)
    {
        const std::uint64_t callback_us = local_now_us();
        if (timestamp.QuadPart <= 0) return callback_us;
        LARGE_INTEGER now{};
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceCounter(&now) || !QueryPerformanceFrequency(&frequency) ||
            frequency.QuadPart <= 0 || now.QuadPart < timestamp.QuadPart)
            return callback_us;
        const long double age = static_cast<long double>(now.QuadPart - timestamp.QuadPart) * 1000000.0L /
                                static_cast<long double>(frequency.QuadPart);
        if (age < 0.0L || age > static_cast<long double>(callback_us)) return callback_us;
        return callback_us - static_cast<std::uint64_t>(age);
    }

    static std::uint32_t pack_rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a)
    {
        return static_cast<std::uint32_t>(r) |
               (static_cast<std::uint32_t>(g) << 8) |
               (static_cast<std::uint32_t>(b) << 16) |
               (static_cast<std::uint32_t>(a) << 24);
    }

    bool preprocess_shape(PlatformError& error)
    {
        shape_width_ = static_cast<int>(shape_info_.Width);
        shape_height_ = static_cast<int>(shape_info_.Height);
        if (shape_width_ <= 0 || shape_height_ <= 0 || shape_info_.Pitch == 0) {
            error = {PlatformComponent::Capture, PlatformFailure::InvalidState,
                     "DXGI pointer shape has invalid dimensions", true};
            return false;
        }

        if (shape_info_.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
            if ((shape_height_ & 1) != 0) return false;
            shape_height_ /= 2;
            const std::size_t required = static_cast<std::size_t>(shape_info_.Pitch) *
                                         static_cast<std::size_t>(shape_height_) * 2u;
            if (required > shape_buffer_.size()) return false;
            ops_.assign(static_cast<std::size_t>(shape_width_) * static_cast<std::size_t>(shape_height_), {});
            for (int y = 0; y < shape_height_; ++y) {
                const auto* and_row = shape_buffer_.data() + static_cast<std::size_t>(y) * shape_info_.Pitch;
                const auto* xor_row = shape_buffer_.data() + static_cast<std::size_t>(y + shape_height_) * shape_info_.Pitch;
                for (int x = 0; x < shape_width_; ++x) {
                    const std::uint8_t bit = static_cast<std::uint8_t>(0x80u >> (x & 7));
                    const bool and_bit = (and_row[x / 8] & bit) != 0;
                    const bool xor_bit = (xor_row[x / 8] & bit) != 0;
                    auto& op = ops_[static_cast<std::size_t>(y) * shape_width_ + static_cast<std::size_t>(x)];
                    op.flags = 4u | (and_bit ? 0x100u : 0u) | (xor_bit ? 0x200u : 0u);
                }
            }
            return true;
        }

        if (shape_info_.Type != DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR &&
            shape_info_.Type != DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
            error = {PlatformComponent::Capture, PlatformFailure::Unsupported,
                     "DXGI returned an unknown pointer shape type", true};
            return false;
        }

        const std::size_t required = static_cast<std::size_t>(shape_info_.Pitch) * static_cast<std::size_t>(shape_height_);
        if (required > shape_buffer_.size() || shape_info_.Pitch < static_cast<UINT>(shape_width_ * 4)) return false;
        ops_.assign(static_cast<std::size_t>(shape_width_) * static_cast<std::size_t>(shape_height_), {});
        for (int y = 0; y < shape_height_; ++y) {
            const auto* row = shape_buffer_.data() + static_cast<std::size_t>(y) * shape_info_.Pitch;
            for (int x = 0; x < shape_width_; ++x) {
                const auto* pixel = row + static_cast<std::size_t>(x) * 4u;
                const std::uint8_t b = pixel[0], g = pixel[1], r = pixel[2], a = pixel[3];
                auto& op = ops_[static_cast<std::size_t>(y) * shape_width_ + static_cast<std::size_t>(x)];
                op.color = pack_rgba(r, g, b, a);
                if (shape_info_.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR)
                    op.flags = 1u;
                else
                    op.flags = a == 0 ? 2u : 3u;
            }
        }
        return true;
    }

    bool ensure_shaders(ID3D11Device* device, PlatformError& error)
    {
        if (vertex_shader_ && pixel_shader_ && cursor_constants_) return true;

        static constexpr char vertex_source[] = R"hlsl(
struct VSOut { float4 position : SV_Position; };
VSOut main(uint id : SV_VertexID)
{
    float2 p = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.position = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    return o;
}
)hlsl";
        static constexpr char pixel_source[] = R"hlsl(
Texture2D<float4> Desktop : register(t0);
Texture2D<uint2> CursorOps : register(t1);
cbuffer CursorConstants : register(b0)
{
    uint DesktopWidth;
    uint DesktopHeight;
    int CursorLeft;
    int CursorTop;
    uint CursorWidth;
    uint CursorHeight;
    uint ShapeWidth;
    uint ShapeHeight;
};

float4 main(float4 position : SV_Position) : SV_Target
{
    int2 pixel = int2(position.xy);
    float4 background = Desktop.Load(int3(pixel, 0));
    int2 local = pixel - int2(CursorLeft, CursorTop);
    uint sx = min(ShapeWidth - 1, (uint(max(local.x, 0)) * ShapeWidth) / max(CursorWidth, 1u));
    uint sy = min(ShapeHeight - 1, (uint(max(local.y, 0)) * ShapeHeight) / max(CursorHeight, 1u));
    uint2 op = CursorOps.Load(int3(int2(sx, sy), 0));
    uint mode = op.y & 0xffu;
    uint r = op.x & 0xffu;
    uint g = (op.x >> 8) & 0xffu;
    uint b = (op.x >> 16) & 0xffu;
    uint a = (op.x >> 24) & 0xffu;

    if (mode == 1u) {
        float alpha = float(a) / 255.0;
        float3 foreground = float3(r, g, b) / 255.0;
        return float4(lerp(background.rgb, foreground, alpha), 1.0);
    }
    if (mode == 2u)
        return float4(float3(r, g, b) / 255.0, 1.0);

    uint3 bg = uint3(round(saturate(background.rgb) * 255.0));
    if (mode == 3u) {
        uint3 value = bg ^ uint3(r, g, b);
        return float4(float3(value) / 255.0, 1.0);
    }
    if (mode == 4u) {
        uint andMask = (op.y & 0x100u) != 0u ? 255u : 0u;
        uint xorMask = (op.y & 0x200u) != 0u ? 255u : 0u;
        uint3 value = (bg & uint3(andMask, andMask, andMask)) ^ uint3(xorMask, xorMask, xorMask);
        return float4(float3(value) / 255.0, 1.0);
    }
    return background;
}
)hlsl";

        ID3DBlob* vertex_blob = nullptr;
        ID3DBlob* pixel_blob = nullptr;
        ID3DBlob* messages = nullptr;
        const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
        HRESULT hr = D3DCompile(vertex_source, sizeof(vertex_source) - 1, "opal-cursor-vs", nullptr, nullptr,
                                "main", "vs_5_0", flags, 0, &vertex_blob, &messages);
        cursor_release(messages);
        if (SUCCEEDED(hr))
            hr = D3DCompile(pixel_source, sizeof(pixel_source) - 1, "opal-cursor-ps", nullptr, nullptr,
                            "main", "ps_5_0", flags, 0, &pixel_blob, &messages);
        cursor_release(messages);
        if (SUCCEEDED(hr)) hr = device->CreateVertexShader(vertex_blob->GetBufferPointer(), vertex_blob->GetBufferSize(), nullptr, &vertex_shader_);
        if (SUCCEEDED(hr)) hr = device->CreatePixelShader(pixel_blob->GetBufferPointer(), pixel_blob->GetBufferSize(), nullptr, &pixel_shader_);
        cursor_release(vertex_blob);
        cursor_release(pixel_blob);
        if (FAILED(hr)) {
            error = cursor_error(hr, "D3D11 cursor shader creation failed");
            return false;
        }

        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = sizeof(CursorConstants);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = device->CreateBuffer(&desc, nullptr, &cursor_constants_);
        if (FAILED(hr) || !cursor_constants_) {
            error = cursor_error(hr, "D3D11 cursor constant buffer creation failed");
            return false;
        }
        return true;
    }

    bool ensure_base_resources(ID3D11Device* device, ID3D11Texture2D* base, PlatformError& error)
    {
        if (base_texture_ == base && base_srv_ && cursor_target_texture_ && cursor_target_rtv_) return true;
        cursor_release(base_srv_);
        cursor_release(base_texture_);
        cursor_release(cursor_target_rtv_);
        cursor_release(cursor_target_texture_);

        D3D11_TEXTURE2D_DESC source{};
        base->GetDesc(&source);
        if (source.Format != DXGI_FORMAT_B8G8R8A8_UNORM || source.Width == 0 || source.Height == 0) {
            error = {PlatformComponent::Capture, PlatformFailure::Unsupported,
                     "cursor compositor requires a BGRA8 desktop surface", false};
            return false;
        }

        HRESULT hr = device->CreateShaderResourceView(base, nullptr, &base_srv_);
        if (FAILED(hr) || !base_srv_) {
            error = cursor_error(hr, "D3D11 desktop cursor SRV creation failed");
            return false;
        }
        base_texture_ = base;
        base_texture_->AddRef();

        D3D11_TEXTURE2D_DESC target = source;
        target.MipLevels = 1;
        target.ArraySize = 1;
        target.Usage = D3D11_USAGE_DEFAULT;
        target.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        target.CPUAccessFlags = 0;
        target.MiscFlags = 0;
        hr = device->CreateTexture2D(&target, nullptr, &cursor_target_texture_);
        if (SUCCEEDED(hr)) hr = device->CreateRenderTargetView(cursor_target_texture_, nullptr, &cursor_target_rtv_);
        if (FAILED(hr) || !cursor_target_texture_ || !cursor_target_rtv_) {
            error = cursor_error(hr, "D3D11 cursor target creation failed");
            return false;
        }
        return true;
    }

    bool ensure_cursor_texture(ID3D11Device* device, PlatformError& error)
    {
        if (cursor_ops_texture_ && cursor_ops_srv_ && uploaded_generation_ == shape_generation_) return true;
        cursor_release(cursor_ops_srv_);
        cursor_release(cursor_ops_texture_);
        if (ops_.empty() || shape_width_ <= 0 || shape_height_ <= 0) return false;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(shape_width_);
        desc.Height = static_cast<UINT>(shape_height_);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32_UINT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{};
        initial.pSysMem = ops_.data();
        initial.SysMemPitch = static_cast<UINT>(static_cast<std::size_t>(shape_width_) * sizeof(CursorOp));
        HRESULT hr = device->CreateTexture2D(&desc, &initial, &cursor_ops_texture_);
        if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(cursor_ops_texture_, nullptr, &cursor_ops_srv_);
        if (FAILED(hr) || !cursor_ops_texture_ || !cursor_ops_srv_) {
            error = cursor_error(hr, "D3D11 cursor operation texture creation failed");
            return false;
        }
        uploaded_generation_ = shape_generation_;
        return true;
    }

    std::vector<std::uint8_t> shape_buffer_;
    std::vector<CursorOp> ops_;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape_info_{};
    POINT position_{};
    bool visible_ = false;
    bool shape_valid_ = false;
    std::size_t who_updated_ = std::numeric_limits<std::size_t>::max();
    LONGLONG last_timestamp_ = 0;
    std::uint64_t capture_time_us_ = 0;
    std::uint64_t shape_generation_ = 0;
    std::uint64_t uploaded_generation_ = 0;
    int shape_width_ = 0;
    int shape_height_ = 0;

    ID3D11Texture2D* base_texture_ = nullptr;
    ID3D11ShaderResourceView* base_srv_ = nullptr;
    ID3D11Texture2D* cursor_target_texture_ = nullptr;
    ID3D11RenderTargetView* cursor_target_rtv_ = nullptr;
    ID3D11Texture2D* cursor_ops_texture_ = nullptr;
    ID3D11ShaderResourceView* cursor_ops_srv_ = nullptr;
    ID3D11VertexShader* vertex_shader_ = nullptr;
    ID3D11PixelShader* pixel_shader_ = nullptr;
    ID3D11Buffer* cursor_constants_ = nullptr;
};

} // namespace opal::windows_detail
