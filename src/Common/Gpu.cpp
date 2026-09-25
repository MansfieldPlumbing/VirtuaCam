// =============================================================================
// Gpu.cpp  --  Direct3D 11 helpers
// =============================================================================

#include "Gpu.h"
#include "NoSignalMask.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Gpu {

HRESULT CreateDevice(ID3D11Device** device, UINT extraFlags)
{
    RETURN_HR_IF_NULL(E_POINTER, device);
    *device = nullptr;
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    wil::com_ptr_nothrow<ID3D11Device> created;
    RETURN_IF_FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | extraFlags, levels, ARRAYSIZE(levels),
        D3D11_SDK_VERSION, &created, nullptr, nullptr));

    wil::com_ptr_nothrow<ID3D10Multithread> multithread;
    if (SUCCEEDED(created->QueryInterface(IID_PPV_ARGS(&multithread))))
        multithread->SetMultithreadProtected(TRUE);

    *device = created.detach();
    return S_OK;
}

HRESULT CompileShader(const char* source, const char* entry, const char* target, ID3DBlob** blob)
{
    wil::com_ptr_nothrow<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &errors);
    if (FAILED(hr) && errors)
        OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
    return hr;
}

HRESULT CreateVertexShader(ID3D11Device* device, const char* source, const char* entry, ID3D11VertexShader** shader)
{
    wil::com_ptr_nothrow<ID3DBlob> blob;
    RETURN_IF_FAILED(CompileShader(source, entry, "vs_5_0", &blob));
    return device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, shader);
}

HRESULT CreatePixelShader(ID3D11Device* device, const char* source, const char* entry, ID3D11PixelShader** shader)
{
    wil::com_ptr_nothrow<ID3DBlob> blob;
    RETURN_IF_FAILED(CompileShader(source, entry, "ps_5_0", &blob));
    return device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, shader);
}

HRESULT CreateLinearClampSampler(ID3D11Device* device, ID3D11SamplerState** sampler)
{
    D3D11_SAMPLER_DESC desc{};
    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    desc.MaxLOD = D3D11_FLOAT32_MAX;
    return device->CreateSamplerState(&desc, sampler);
}

RectF FitRect(const RectF& dst, UINT srcWidth, UINT srcHeight)
{
    if (!srcWidth || !srcHeight || dst.w <= 0 || dst.h <= 0)
        return dst;
    const float scale = std::min(dst.w / srcWidth, dst.h / srcHeight);
    const float w = srcWidth * scale;
    const float h = srcHeight * scale;
    return { dst.x + (dst.w - w) * 0.5f, dst.y + (dst.h - h) * 0.5f, w, h };
}

namespace {
// Bilinear sample of the wordmark alpha mask, 0..1.
float SampleMask(float x, float y)
{
    if (x < 0 || y < 0 || x > kNoSignalMaskWidth - 1.0f || y > kNoSignalMaskHeight - 1.0f)
        return 0.0f;
    const UINT x0 = (UINT)x, y0 = (UINT)y;
    const UINT x1 = std::min(x0 + 1, kNoSignalMaskWidth - 1);
    const UINT y1 = std::min(y0 + 1, kNoSignalMaskHeight - 1);
    const float fx = x - x0, fy = y - y0;
    auto at = [](UINT px, UINT py) { return (float)kNoSignalMask[py * kNoSignalMaskWidth + px]; };
    const float top = at(x0, y0) * (1 - fx) + at(x1, y0) * fx;
    const float bottom = at(x0, y1) * (1 - fx) + at(x1, y1) * fx;
    return (top * (1 - fy) + bottom * fy) / 255.0f;
}
}

HRESULT CreateNoSignalTexture(ID3D11Device* device, UINT width, UINT height, ID3D11Texture2D** texture)
{
    RETURN_HR_IF_NULL(E_POINTER, device);
    RETURN_HR_IF_NULL(E_POINTER, texture);
    *texture = nullptr;
    RETURN_HR_IF(E_INVALIDARG, !width || !height);

    // Background: cool dark grey at the centre falling off to near-black.
    std::vector<uint32_t> pixels((size_t)width * height);
    const float cx = width * 0.5f, cy = height * 0.5f;
    const float invMax = 1.0f / std::sqrt(cx * cx + cy * cy);
    for (UINT y = 0; y < height; y++) {
        for (UINT x = 0; x < width; x++) {
            const float dx = x - cx, dy = y - cy;
            float t = std::sqrt(dx * dx + dy * dy) * invMax;
            t = t * t * (3.0f - 2.0f * t);
            const uint32_t r = (uint32_t)(24.0f + (7.0f - 24.0f) * t);
            const uint32_t g = (uint32_t)(26.0f + (8.0f - 26.0f) * t);
            const uint32_t b = (uint32_t)(30.0f + (10.0f - 30.0f) * t);
            pixels[(size_t)y * width + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }

    // Wordmark: 1/12 of the frame height, at most half the width.
    float scale = (height / 12.0f) / kNoSignalMaskHeight;
    if (kNoSignalMaskWidth * scale > width * 0.5f)
        scale = (width * 0.5f) / kNoSignalMaskWidth;
    const UINT outW = std::max(1u, (UINT)(kNoSignalMaskWidth * scale));
    const UINT outH = std::max(1u, (UINT)(kNoSignalMaskHeight * scale));
    const UINT ox = width > outW ? (width - outW) / 2 : 0;
    const UINT oy = height > outH ? (height - outH) / 2 : 0;
    for (UINT y = 0; y < outH && oy + y < height; y++) {
        for (UINT x = 0; x < outW && ox + x < width; x++) {
            const float a = SampleMask(x / scale, y / scale);
            if (a <= 0.0f)
                continue;
            uint32_t& dst = pixels[(size_t)(oy + y) * width + (ox + x)];
            auto mix = [a](uint32_t bg, float fg) { return (uint32_t)(bg + (fg - bg) * a); };
            const uint32_t r = mix((dst >> 16) & 0xFF, 0x8A);
            const uint32_t g = mix((dst >> 8) & 0xFF, 0x8F);
            const uint32_t b = mix(dst & 0xFF, 0x98);
            dst = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    // DEFAULT rather than IMMUTABLE so it is also accepted as video-processor input.
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    const D3D11_SUBRESOURCE_DATA init{ pixels.data(), width * 4, 0 };
    return device->CreateTexture2D(&desc, &init, texture);
}

} // namespace Gpu
