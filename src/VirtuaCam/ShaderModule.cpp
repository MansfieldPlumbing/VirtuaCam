#include "pch.h"
#include "ShaderModule.h"
#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

ShaderModule::ShaderModule() {}
ShaderModule::~ShaderModule() {
    Shutdown();
}

HRESULT ShaderModule::Initialize(wil::com_ptr_nothrow<ID3D11Device> device) {
    m_device = device;
    m_device->GetImmediateContext(&m_context);

    wil::com_ptr_nothrow<ID3DBlob> vsBlob, psBlob;
    HRESULT hr = D3DCompile(g_OffModeVertexShader, strlen(g_OffModeVertexShader), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, nullptr);
    if (FAILED(hr)) return hr;
    hr = D3DCompile(g_OffModePixelShader, strlen(g_OffModePixelShader), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, nullptr);
    if (FAILED(hr)) return hr;

    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vs);
    if (FAILED(hr)) return hr;
    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_ps);
    if (FAILED(hr)) return hr;

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD = 0;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    hr = m_device->CreateSamplerState(&sd, &m_sampler);
    return hr;
}

void ShaderModule::Shutdown() {
    if (m_context) m_context->ClearState();
    m_vs.reset();
    m_ps.reset();
    m_sampler.reset();
    m_context.reset();
    m_device.reset();
}

void ShaderModule::Render(wil::com_ptr_nothrow<ID3D11RenderTargetView> rtv, UINT width, UINT height) {
    if (!m_context || !m_vs || !m_ps || !rtv) return;

    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
    m_context->RSSetViewports(1, &vp);
    m_context->OMSetRenderTargets(1, rtv.addressof(), nullptr);
    
    m_context->VSSetShader(m_vs.get(), nullptr, 0);
    m_context->PSSetShader(m_ps.get(), nullptr, 0);
    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->Draw(3, 0);
}