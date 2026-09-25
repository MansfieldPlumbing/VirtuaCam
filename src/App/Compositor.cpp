// =============================================================================
// Compositor.cpp
// =============================================================================

#include "Compositor.h"

namespace {

struct LayerConstants {
    float rect[4];     // content x, y, w, h (pixels)
    float target[2];   // output width, height
    float radius;
    float shadow;
    float outline;
    float pad[3];
};
static_assert(sizeof(LayerConstants) % 16 == 0, "constant buffer size");

// The quad covers the content rect grown by the shadow radius; everything
// else is decided per pixel from the distance to the rounded rectangle.
const char kShader[] = R"(
cbuffer Layer : register(b0) {
    float4 Rect;      // x, y, w, h
    float2 Target;
    float  Radius;
    float  Shadow;
    float  Outline;
};
Texture2D    Source  : register(t0);
SamplerState Linear  : register(s0);

struct Fragment { float4 position : SV_Position; float2 pixel : PIXEL; };

Fragment VS(uint id : SV_VertexID) {
    float2 corner = float2(id & 1, id >> 1);
    float2 grow = Shadow.xx * float2(1.0, 1.35);
    float2 pixel = Rect.xy - grow + corner * (Rect.zw + 2.0 * grow);
    Fragment f;
    f.pixel = pixel;
    f.position = float4(pixel / Target * float2(2, -2) + float2(-1, 1), 0, 1);
    return f;
}

float RoundedBox(float2 p, float2 halfSize, float r) {
    float2 q = abs(p) - halfSize + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float4 PS(Fragment f) : SV_Target {
    float2 halfSize = Rect.zw * 0.5;
    float2 p = f.pixel - (Rect.xy + halfSize);
    float  r = min(Radius, min(halfSize.x, halfSize.y));
    float  d = RoundedBox(p, halfSize, r);

    float coverage = saturate(0.5 - d);
    float2 uv = saturate((f.pixel - Rect.xy) / Rect.zw);
    float3 color = Source.Sample(Linear, uv).rgb;

    // Light rim just inside the edge.
    color = lerp(color, float3(1, 1, 1), Outline * saturate(1.0 - abs(d + 1.0)));

    // Soft shadow, dropped slightly downward, fading with distance.
    float shadow = 0.0;
    if (Shadow > 0.0) {
        float ds = RoundedBox(p - float2(0.0, Shadow * 0.35), halfSize, r);
        float t = saturate(1.0 - ds / Shadow);
        shadow = 0.45 * t * t;
    }

    // Premultiplied: content over its own shadow.
    float alpha = coverage + shadow * (1.0 - coverage);
    return float4(color * coverage, alpha);
}
)";

} // namespace

HRESULT Compositor::Initialize(ID3D11Device* device)
{
    m_device = device;
    RETURN_IF_FAILED(Gpu::CreateVertexShader(device, kShader, "VS", &m_vertexShader));
    RETURN_IF_FAILED(Gpu::CreatePixelShader(device, kShader, "PS", &m_pixelShader));
    RETURN_IF_FAILED(Gpu::CreateLinearClampSampler(device, &m_sampler));

    D3D11_BLEND_DESC blend{};
    auto& rt = blend.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_ONE;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    RETURN_IF_FAILED(device->CreateBlendState(&blend, &m_blend));

    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    RETURN_IF_FAILED(device->CreateRasterizerState(&raster, &m_rasterizer));

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(LayerConstants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return device->CreateBuffer(&cb, nullptr, &m_constants);
}

void Compositor::DrawLayer(ID3D11DeviceContext* context, const CompositorLayer& layer, UINT width, UINT height)
{
    if (!layer.view || layer.rect.w <= 0 || layer.rect.h <= 0)
        return;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(m_constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return;
    LayerConstants c{};
    c.rect[0] = layer.rect.x;
    c.rect[1] = layer.rect.y;
    c.rect[2] = layer.rect.w;
    c.rect[3] = layer.rect.h;
    c.target[0] = (float)width;
    c.target[1] = (float)height;
    c.radius = layer.cornerRadius;
    c.shadow = layer.shadowRadius;
    c.outline = layer.outlineOpacity;
    memcpy(mapped.pData, &c, sizeof(c));
    context->Unmap(m_constants.get(), 0);

    ID3D11ShaderResourceView* views[] = { layer.view };
    context->PSSetShaderResources(0, 1, views);
    context->Draw(4, 0);
}

void Compositor::Draw(ID3D11RenderTargetView* target, UINT width, UINT height,
                      ID3D11ShaderResourceView* background, UINT backgroundWidth, UINT backgroundHeight,
                      const std::vector<CompositorLayer>& layers)
{
    wil::com_ptr_nothrow<ID3D11DeviceContext> context;
    m_device->GetImmediateContext(&context);

    const float clear[] = { 0.02f, 0.02f, 0.025f, 1.0f };
    context->ClearRenderTargetView(target, clear);

    const D3D11_VIEWPORT viewport{ 0, 0, (float)width, (float)height, 0, 1 };
    context->RSSetViewports(1, &viewport);
    context->RSSetState(m_rasterizer.get());
    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(m_blend.get(), nullptr, 0xFFFFFFFF);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->VSSetShader(m_vertexShader.get(), nullptr, 0);
    context->VSSetConstantBuffers(0, 1, m_constants.addressof());
    context->PSSetShader(m_pixelShader.get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, m_constants.addressof());
    context->PSSetSamplers(0, 1, m_sampler.addressof());

    if (background) {
        CompositorLayer layer;
        layer.view = background;
        layer.rect = Gpu::FitRect({ 0, 0, (float)width, (float)height }, backgroundWidth, backgroundHeight);
        DrawLayer(context.get(), layer, width, height);
    }
    for (const auto& layer : layers)
        DrawLayer(context.get(), layer, width, height);

    ID3D11ShaderResourceView* none[] = { nullptr };
    context->PSSetShaderResources(0, 1, none);
    context->OMSetRenderTargets(0, nullptr, nullptr);
}
