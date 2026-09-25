// =============================================================================
// Compositor.h  --  Draws the camera frame from its layers
// =============================================================================
// A frame is a background plus an ordered list of layers.  Each layer is one
// textured, rounded-corner rectangle with an optional soft drop shadow, drawn
// with a single draw call: the pixel shader evaluates a signed-distance
// rounded box, so edges are anti-aliased and shadows need no extra passes or
// intermediate targets.
// =============================================================================

#pragma once

#include "Gpu.h"

#include <d3d11_4.h>
#include <wil/com.h>
#include <vector>

struct CompositorLayer {
    ID3D11ShaderResourceView* view = nullptr;
    Gpu::RectF rect{};            // destination in output pixels (already aspect-fitted)
    float cornerRadius = 0.0f;    // pixels
    float shadowRadius = 0.0f;    // pixels; 0 disables the shadow
    float outlineOpacity = 0.0f;  // faint light edge for overlays on dark video
};

class Compositor {
public:
    HRESULT Initialize(ID3D11Device* device);

    // Renders into `target` (BGRA render target of size width x height).
    // background may be null (solid near-black).
    void Draw(ID3D11RenderTargetView* target, UINT width, UINT height,
              ID3D11ShaderResourceView* background, UINT backgroundWidth, UINT backgroundHeight,
              const std::vector<CompositorLayer>& layers);

private:
    void DrawLayer(ID3D11DeviceContext* context, const CompositorLayer& layer, UINT width, UINT height);

    wil::com_ptr_nothrow<ID3D11Device>        m_device;
    wil::com_ptr_nothrow<ID3D11VertexShader>  m_vertexShader;
    wil::com_ptr_nothrow<ID3D11PixelShader>   m_pixelShader;
    wil::com_ptr_nothrow<ID3D11SamplerState>  m_sampler;
    wil::com_ptr_nothrow<ID3D11BlendState>    m_blend;
    wil::com_ptr_nothrow<ID3D11RasterizerState> m_rasterizer;
    wil::com_ptr_nothrow<ID3D11Buffer>        m_constants;
};
