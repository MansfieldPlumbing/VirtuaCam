#pragma once
#include <windows.h>
#include <d3d11_4.h>
#include "wil/com.h" // Use WIL for smart pointers

class ShaderModule {
public:
    ShaderModule();
    ~ShaderModule();

    HRESULT Initialize(wil::com_ptr_nothrow<ID3D11Device> device);
    void Shutdown();
    void Render(wil::com_ptr_nothrow<ID3D11RenderTargetView> rtv, UINT width, UINT height);

private:
    wil::com_ptr_nothrow<ID3D11Device> m_device;
    wil::com_ptr_nothrow<ID3D11DeviceContext> m_context;
    wil::com_ptr_nothrow<ID3D11VertexShader> m_vs;
    wil::com_ptr_nothrow<ID3D11PixelShader> m_ps;
    wil::com_ptr_nothrow<ID3D11SamplerState> m_sampler;

    const char* g_OffModeVertexShader = R"(
struct VS_OUTPUT {
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD;
};
VS_OUTPUT main(uint id : SV_VertexID) {
    VS_OUTPUT output;
    output.Tex = float2((id << 1) & 2, id & 2);
    output.Pos = float4(output.Tex.x * 2.0 - 1.0, 1.0 - output.Tex.y * 2.0, 0, 1);
    return output;
}
)";

    const char* g_OffModePixelShader = R"(
float4 main(float4 Pos : SV_POSITION, float2 Tex : TEXCOORD) : SV_TARGET {
    return float4(0.1 + Tex.x * 0.2, 0.1 + Tex.y * 0.2, 0.4, 1.0);
}
)";
};