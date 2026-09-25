// =============================================================================
// Presenter.cpp
// =============================================================================

#include "Presenter.h"

#include <algorithm>
#include <cmath>

namespace {

// Full-target triangle; the pixel shader averages a 4x4 grid of bilinear taps
// spread over each output pixel's footprint in the source, which keeps small
// thumbnails of large frames from shimmering.
const char kShader[] = R"(
cbuffer Params : register(b0) { float2 Footprint; float2 Pad; };
Texture2D    Source : register(t0);
SamplerState Linear : register(s0);

struct Fragment { float4 position : SV_Position; float2 uv : UV; };

Fragment VS(uint id : SV_VertexID) {
    Fragment f;
    f.uv = float2((id << 1) & 2, id & 2);
    f.position = float4(f.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return f;
}

float4 PS(Fragment f) : SV_Target {
    float3 sum = 0;
    [unroll] for (int y = 0; y < 4; y++)
        [unroll] for (int x = 0; x < 4; x++)
            sum += Source.SampleLevel(Linear, f.uv + (float2(x, y) - 1.5) * 0.25 * Footprint, 0).rgb;
    return float4(sum / 16.0, 1.0);
}
)";

} // namespace

void Presenter::Detach()
{
    if (m_visual)
        m_visual->SetContent(nullptr);
    if (m_composition)
        m_composition->Commit();
    m_backBuffer.reset();
    m_swapChain.reset();
    m_clip.reset();
    m_visual.reset();
    m_target.reset();
    m_composition.reset();
    m_device.reset();
    m_hwnd = nullptr;
    m_width = m_height = 0;
    m_cornerRadius = -1.0f;
}

HRESULT Presenter::EnsureComposition(ID3D11Device* device)
{
    if (m_composition && m_device.get() == device)
        return S_OK;
    const HWND hwnd = m_hwnd;
    const bool topmost = m_topmost;
    Detach();
    m_hwnd = hwnd;
    m_topmost = topmost;
    m_device = device;

    wil::com_ptr_nothrow<IDXGIDevice> dxgi;
    RETURN_IF_FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi)));
    RETURN_IF_FAILED(DCompositionCreateDevice(dxgi.get(), IID_PPV_ARGS(&m_composition)));
    RETURN_IF_FAILED(m_composition->CreateTargetForHwnd(m_hwnd, m_topmost, &m_target));
    RETURN_IF_FAILED(m_composition->CreateVisual(&m_visual));
    RETURN_IF_FAILED(m_composition->CreateRectangleClip(&m_clip));
    RETURN_IF_FAILED(m_visual->SetClip(m_clip.get()));
    RETURN_IF_FAILED(m_target->SetRoot(m_visual.get()));

    RETURN_IF_FAILED(Gpu::CreateVertexShader(device, kShader, "VS", &m_vertexShader));
    RETURN_IF_FAILED(Gpu::CreatePixelShader(device, kShader, "PS", &m_pixelShader));
    RETURN_IF_FAILED(Gpu::CreateLinearClampSampler(device, &m_sampler));
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = 16;
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return device->CreateBuffer(&cb, nullptr, &m_constants);
}

HRESULT Presenter::EnsureSwapChain(UINT width, UINT height)
{
    if (m_swapChain && width == m_width && height == m_height)
        return S_OK;
    m_backBuffer.reset();
    if (m_swapChain) {
        RETURN_IF_FAILED(m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0));
    } else {
        wil::com_ptr_nothrow<IDXGIDevice> dxgi;
        wil::com_ptr_nothrow<IDXGIAdapter> adapter;
        wil::com_ptr_nothrow<IDXGIFactory2> factory;
        RETURN_IF_FAILED(m_device->QueryInterface(IID_PPV_ARGS(&dxgi)));
        RETURN_IF_FAILED(dxgi->GetAdapter(&adapter));
        RETURN_IF_FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)));

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        RETURN_IF_FAILED(factory->CreateSwapChainForComposition(m_device.get(), &desc, nullptr, &m_swapChain));
        RETURN_IF_FAILED(m_visual->SetContent(m_swapChain.get()));
    }
    wil::com_ptr_nothrow<ID3D11Texture2D> buffer;
    RETURN_IF_FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer)));
    RETURN_IF_FAILED(m_device->CreateRenderTargetView(buffer.get(), nullptr, &m_backBuffer));
    m_width = width;
    m_height = height;
    return S_OK;
}

HRESULT Presenter::Present(ID3D11ShaderResourceView* view, UINT sourceWidth, UINT sourceHeight,
                           const RECT& bounds, float cornerRadius)
{
    RETURN_HR_IF(E_UNEXPECTED, !m_hwnd);
    RETURN_HR_IF_NULL(E_POINTER, view);

    wil::com_ptr_nothrow<ID3D11Device> device;
    view->GetDevice(&device);
    RETURN_IF_FAILED(EnsureComposition(device.get()));

    const Gpu::RectF fit = Gpu::FitRect({ (float)bounds.left, (float)bounds.top,
        (float)(bounds.right - bounds.left), (float)(bounds.bottom - bounds.top) }, sourceWidth, sourceHeight);
    const UINT width = std::max(1u, (UINT)std::lround(fit.w));
    const UINT height = std::max(1u, (UINT)std::lround(fit.h));
    const bool geometryChanged = width != m_width || height != m_height ||
        std::lround(fit.x) != std::lround(m_videoRect.x) || std::lround(fit.y) != std::lround(m_videoRect.y) ||
        cornerRadius != m_cornerRadius;
    RETURN_IF_FAILED(EnsureSwapChain(width, height));
    m_videoRect = fit;

    wil::com_ptr_nothrow<ID3D11DeviceContext> context;
    m_device->GetImmediateContext(&context);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    RETURN_IF_FAILED(context->Map(m_constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
    const float footprint[4] = { 1.0f / width, 1.0f / height, 0, 0 };
    memcpy(mapped.pData, footprint, sizeof(footprint));
    context->Unmap(m_constants.get(), 0);

    const D3D11_VIEWPORT viewport{ 0, 0, (float)width, (float)height, 0, 1 };
    ID3D11RenderTargetView* target = m_backBuffer.get();
    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context->RSSetState(nullptr);
    context->RSSetViewports(1, &viewport);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vertexShader.get(), nullptr, 0);
    context->PSSetShader(m_pixelShader.get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, m_constants.addressof());
    context->PSSetSamplers(0, 1, m_sampler.addressof());
    context->PSSetShaderResources(0, 1, &view);
    context->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    context->PSSetShaderResources(0, 1, &none);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    RETURN_IF_FAILED(m_swapChain->Present(0, 0));

    if (geometryChanged) {
        m_visual->SetOffsetX(std::round(fit.x));
        m_visual->SetOffsetY(std::round(fit.y));
        m_clip->SetLeft(0.0f);
        m_clip->SetTop(0.0f);
        m_clip->SetRight((float)width);
        m_clip->SetBottom((float)height);
        const float r = cornerRadius;
        m_clip->SetTopLeftRadiusX(r);     m_clip->SetTopLeftRadiusY(r);
        m_clip->SetTopRightRadiusX(r);    m_clip->SetTopRightRadiusY(r);
        m_clip->SetBottomLeftRadiusX(r);  m_clip->SetBottomLeftRadiusY(r);
        m_clip->SetBottomRightRadiusX(r); m_clip->SetBottomRightRadiusY(r);
        m_cornerRadius = cornerRadius;
        RETURN_IF_FAILED(m_composition->Commit());
    }
    return S_OK;
}
