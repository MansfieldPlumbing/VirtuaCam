#define WIN32_LEAN_AND_MEAN
#include "pch.h"
#include <wrl.h>
#include "Consumer.h"
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <sddl.h>
#include <string>
#include <atomic>
#include <memory>
#include <wil/com.h>
#include <wil/result.h>
#include <wil/resource.h>
#include "Utilities.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "advapi32.lib")

using namespace Microsoft::WRL;

namespace VirtuaCam {

class PipelineNode
{
public:
    ~PipelineNode();
    HRESULT Initialize(DWORD inputPid, const std::wstring& manifestPrefix);
    void Shutdown();
    void ProcessLoop();
    bool IsRunning() { return m_isRunning; }

private:
    HRESULT InitD3D11();
    HRESULT ConnectToProducer(DWORD inputPid);
    HRESULT InitializeSharing(UINT width, UINT height, DXGI_FORMAT format, const std::wstring& manifestPrefix);
    HRESULT LoadAssets();
    void RenderFrame();
    void DisconnectFromProducer();
    void ShutdownSharing();

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11Device1> m_device1;
    ComPtr<ID3D11Device5> m_device5;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<ID3D11DeviceContext4> m_context4;
    LUID m_adapterLuid = {};
    ProducerConnection m_inputProducer;
    ComPtr<ID3D11Texture2D>        m_sharedTex_Out;
    HANDLE                         m_sharedNTHandle_Out = nullptr;
    ComPtr<ID3D11Fence>            m_sharedFence_Out;
    HANDLE                         m_sharedFenceHandle_Out = nullptr;
    UINT64                         m_frameValue_Out = 0;
    HANDLE                         m_hManifest_Out = nullptr;
    BroadcastManifest*             m_pManifestView_Out = nullptr;
    ComPtr<ID3D11VertexShader>     m_vertexShader;
    ComPtr<ID3D11PixelShader>      m_passthroughShader;
    ComPtr<ID3D11SamplerState>     m_samplerState;
    std::atomic<bool> m_isRunning = false;
};

static std::unique_ptr<PipelineNode> g_pipelineNode;

PipelineNode::~PipelineNode() {
    Shutdown();
}

void PipelineNode::Shutdown() {
    if (!m_isRunning.exchange(false)) return;
    DisconnectFromProducer();
    ShutdownSharing();
}

HRESULT PipelineNode::Initialize(DWORD inputPid, const std::wstring& manifestPrefix)
{
    RETURN_IF_FAILED(InitD3D11());
    RETURN_IF_FAILED(LoadAssets());
    RETURN_IF_FAILED(ConnectToProducer(inputPid));
    
    D3D11_TEXTURE2D_DESC inputDesc;
    m_inputProducer.sharedTexture->GetDesc(&inputDesc);

    RETURN_IF_FAILED(InitializeSharing(inputDesc.Width, inputDesc.Height, inputDesc.Format, manifestPrefix));

    m_isRunning = true;
    return S_OK;
}

void PipelineNode::ProcessLoop()
{
    if (!m_isRunning) return;

    while (m_isRunning)
    {
        HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, m_inputProducer.producerPid);
        if (hProcess == NULL || WaitForSingleObject(hProcess, 0) != WAIT_TIMEOUT) {
            Shutdown();
            break;
        }
        if(hProcess) CloseHandle(hProcess);
        
        RenderFrame();
        Sleep(1); 
    }
}

void PipelineNode::RenderFrame()
{
    if (!m_inputProducer.isConnected) return;

    UINT64 latestFrame = m_inputProducer.pManifestView->frameValue;
    if (latestFrame > m_inputProducer.lastSeenFrame) {
        m_context4->Wait(m_inputProducer.sharedFence.Get(), latestFrame);
        m_context->CopyResource(m_inputProducer.privateTexture.Get(), m_inputProducer.sharedTexture.Get());
        m_inputProducer.lastSeenFrame = latestFrame;
    }

    ComPtr<ID3D11RenderTargetView> outputRTV;
    m_device->CreateRenderTargetView(m_sharedTex_Out.Get(), nullptr, &outputRTV);
    
    D3D11_TEXTURE2D_DESC outDesc;
    m_sharedTex_Out->GetDesc(&outDesc);
    D3D11_VIEWPORT vp = { 0, 0, (float)outDesc.Width, (float)outDesc.Height, 0, 1 };
    
    m_context->RSSetViewports(1, &vp);
    m_context->OMSetRenderTargets(1, outputRTV.GetAddressOf(), nullptr);
    
    m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    m_context->PSSetShader(m_passthroughShader.Get(), nullptr, 0);
    m_context->PSSetShaderResources(0, 1, m_inputProducer.privateSRV.GetAddressOf());
    m_context->PSSetSamplers(0, 1, m_samplerState.GetAddressOf());
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->Draw(3, 0);

    m_frameValue_Out++;
    m_context4->Signal(m_sharedFence_Out.Get(), m_frameValue_Out);
    if (m_pManifestView_Out) {
        InterlockedExchange64(reinterpret_cast<volatile LONGLONG*>(&m_pManifestView_Out->frameValue), m_frameValue_Out);
    }
}


HRESULT PipelineNode::InitD3D11() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    RETURN_IF_FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &m_device, nullptr, &m_context));
    RETURN_IF_FAILED(m_device.As(&m_device1));
    RETURN_IF_FAILED(m_device.As(&m_device5));
    RETURN_IF_FAILED(m_context.As(&m_context4));

    ComPtr<IDXGIDevice> dxgiDevice;
    m_device.As(&dxgiDevice);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC desc;
    adapter->GetDesc(&desc);
    m_adapterLuid = desc.AdapterLuid;
    
    return S_OK;
}

HRESULT PipelineNode::ConnectToProducer(DWORD inputPid)
{
    DisconnectFromProducer();

    std::wstring manifestName = L"DirectPort_Producer_Manifest_" + std::to_wstring(inputPid);
    HANDLE hManifest = OpenFileMappingW(FILE_MAP_READ, FALSE, manifestName.c_str());
    RETURN_HR_IF(E_FAIL, hManifest == NULL);

    BroadcastManifest* pManifestView = (BroadcastManifest*)MapViewOfFile(hManifest, FILE_MAP_READ, 0, 0, sizeof(BroadcastManifest));
    if (!pManifestView) { CloseHandle(hManifest); return E_FAIL; }
    
    if (memcmp(&pManifestView->adapterLuid, &m_adapterLuid, sizeof(LUID)) != 0) {
        UnmapViewOfFile(pManifestView); CloseHandle(hManifest); return E_FAIL;
    }

    HANDLE hFence = Utils::Win32::GetHandleFromObjectName(pManifestView->fenceName);
    if (!hFence || FAILED(m_device5->OpenSharedFence(hFence, IID_PPV_ARGS(&m_inputProducer.sharedFence)))) {
        if(hFence) CloseHandle(hFence); UnmapViewOfFile(pManifestView); CloseHandle(hManifest); return E_FAIL;
    }
    CloseHandle(hFence);
    
    HANDLE hTexture = Utils::Win32::GetHandleFromObjectName(pManifestView->textureName);
    if (!hTexture || FAILED(m_device1->OpenSharedResource1(hTexture, IID_PPV_ARGS(&m_inputProducer.sharedTexture)))) {
        if(hTexture) CloseHandle(hTexture); UnmapViewOfFile(pManifestView); CloseHandle(hManifest); return E_FAIL;
    }
    CloseHandle(hTexture);

    D3D11_TEXTURE2D_DESC sharedDesc;
    m_inputProducer.sharedTexture->GetDesc(&sharedDesc);
    sharedDesc.MiscFlags = 0;
    sharedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sharedDesc.Usage = D3D11_USAGE_DEFAULT;
    m_device->CreateTexture2D(&sharedDesc, nullptr, &m_inputProducer.privateTexture);
    m_device->CreateShaderResourceView(m_inputProducer.privateTexture.Get(), nullptr, &m_inputProducer.privateSRV);
    
    m_inputProducer.producerPid = inputPid;
    m_inputProducer.hManifest = hManifest;
    m_inputProducer.pManifestView = pManifestView;
    m_inputProducer.isConnected = true;

    return S_OK;
}

void PipelineNode::DisconnectFromProducer() {
    if (!m_inputProducer.isConnected) return;
    if (m_inputProducer.pManifestView) UnmapViewOfFile(m_inputProducer.pManifestView);
    if (m_inputProducer.hManifest) CloseHandle(m_inputProducer.hManifest);
    m_inputProducer = {}; 
}

HRESULT PipelineNode::InitializeSharing(UINT width, UINT height, DXGI_FORMAT format, const std::wstring& manifestPrefix) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width; td.Height = height; td.Format = format;
    td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    RETURN_IF_FAILED(m_device->CreateTexture2D(&td, nullptr, &m_sharedTex_Out));

    RETURN_IF_FAILED(m_device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_sharedFence_Out)));
    
    DWORD pid = GetCurrentProcessId();
    std::wstring textureName = L"Global\\DirectPortTexture_" + std::to_wstring(pid);
    std::wstring fenceName = L"Global\\DirectPortFence_" + std::to_wstring(pid);
    std::wstring manifestName = manifestPrefix + std::to_wstring(pid);

    wil::unique_hlocal_security_descriptor sd;
    PSECURITY_DESCRIPTOR sd_ptr = nullptr;
    THROW_IF_WIN32_BOOL_FALSE(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd_ptr, NULL));
    sd.reset(sd_ptr);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd.get(), FALSE };

    ComPtr<IDXGIResource1> r1; m_sharedTex_Out.As(&r1);
    RETURN_IF_FAILED(r1->CreateSharedHandle(&sa, GENERIC_ALL, textureName.c_str(), &m_sharedNTHandle_Out));
    RETURN_IF_FAILED(m_sharedFence_Out->CreateSharedHandle(&sa, GENERIC_ALL, fenceName.c_str(), &m_sharedFenceHandle_Out));

    m_hManifest_Out = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(BroadcastManifest), manifestName.c_str());
    RETURN_HR_IF(E_FAIL, m_hManifest_Out == NULL);

    m_pManifestView_Out = (BroadcastManifest*)MapViewOfFile(m_hManifest_Out, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BroadcastManifest));
    RETURN_HR_IF(E_FAIL, m_pManifestView_Out == nullptr);
    
    ZeroMemory(m_pManifestView_Out, sizeof(BroadcastManifest));
    m_pManifestView_Out->width = width; m_pManifestView_Out->height = height;
    m_pManifestView_Out->format = format; m_pManifestView_Out->adapterLuid = m_adapterLuid;
    wcscpy_s(m_pManifestView_Out->textureName, textureName.c_str());
    wcscpy_s(m_pManifestView_Out->fenceName, fenceName.c_str());

    return S_OK;
}

void PipelineNode::ShutdownSharing() {
    if (m_pManifestView_Out) UnmapViewOfFile(m_pManifestView_Out);
    if (m_hManifest_Out) CloseHandle(m_hManifest_Out);
    if (m_sharedNTHandle_Out) CloseHandle(m_sharedNTHandle_Out);
    if (m_sharedFenceHandle_Out) CloseHandle(m_sharedFenceHandle_Out);
    m_pManifestView_Out = nullptr; m_hManifest_Out = nullptr;
    m_sharedNTHandle_Out = nullptr;
    m_sharedFenceHandle_Out = nullptr;
    m_sharedTex_Out.Reset(); m_sharedFence_Out.Reset();
}

HRESULT PipelineNode::LoadAssets() {
    const char* vertexShaderHLSL = R"(
    struct VOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
    VOut main(uint vid : SV_VertexID) {
        float2 uv = float2((vid << 1) & 2, vid & 2);
        VOut o; o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1);
        o.uv = uv; return o;
    })";
    const char* passthroughShaderHLSL = R"(
    Texture2D    inputTexture  : register(t0);
    SamplerState linearSampler : register(s0);
    struct VIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
    float4 main(VIn i) : SV_Target { return inputTexture.Sample(linearSampler, i.uv); })";

    ComPtr<ID3DBlob> vsBlob, psBlob;
    RETURN_IF_FAILED(D3DCompile(vertexShaderHLSL, strlen(vertexShaderHLSL), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, nullptr));
    RETURN_IF_FAILED(m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vertexShader));
    
    RETURN_IF_FAILED(D3DCompile(passthroughShaderHLSL, strlen(passthroughShaderHLSL), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, nullptr));
    RETURN_IF_FAILED(m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_passthroughShader));
    
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    RETURN_IF_FAILED(m_device->CreateSamplerState(&sampDesc, &m_samplerState));
    
    return S_OK;
}

}

extern "C" {
    CONSUMER_API HRESULT InitializeProducer(DWORD inputProducerPid, const wchar_t* manifestPrefix)
    {
        VirtuaCam::g_pipelineNode = std::make_unique<VirtuaCam::PipelineNode>();
        return VirtuaCam::g_pipelineNode->Initialize(inputProducerPid, manifestPrefix);
    }

    CONSUMER_API void RunProducer()
    {
        if (VirtuaCam::g_pipelineNode && VirtuaCam::g_pipelineNode->IsRunning())
        {
            VirtuaCam::g_pipelineNode->ProcessLoop();
        }
    }

    CONSUMER_API void ShutdownProducer()
    {
        if (VirtuaCam::g_pipelineNode)
        {
            VirtuaCam::g_pipelineNode->Shutdown();
            VirtuaCam::g_pipelineNode.reset();
        }
    }
}
