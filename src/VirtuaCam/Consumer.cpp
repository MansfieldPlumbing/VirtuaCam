#include "pch.h"
#include "Consumer.h"
#include "Tools.h"
#include "Discovery.h"
#include <wrl.h>
#include <sddl.h>
#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d11.lib")

using namespace Microsoft::WRL;

static ComPtr<ID3D11Device>           g_device;
static ComPtr<ID3D11DeviceContext>    g_context;
static ComPtr<ID3D11DeviceContext4>   g_context4;
static LUID                           g_adapterLuid = {};

static std::unique_ptr<VirtuaCam::Discovery> g_discovery;
static VirtuaCam::DiscoveredSharedStream g_inputStream;
static bool g_inputConnected = false;

static ComPtr<ID3D11Texture2D>        g_inputSharedTexture;
static ComPtr<ID3D11Fence>            g_inputSharedFence;
static ComPtr<ID3D11Texture2D>        g_inputPrivateTexture;
static ComPtr<ID3D11ShaderResourceView> g_inputSRV;
static UINT64                         g_lastSeenFrame = 0;

static ComPtr<ID3D11Texture2D>        g_outputTexture;
static ComPtr<ID3D11RenderTargetView> g_outputRTV;
static ComPtr<ID3D11VertexShader>     g_vs;
static ComPtr<ID3D11PixelShader>      g_ps;
static ComPtr<ID3D11SamplerState>     g_sampler;

static ComPtr<ID3D11Texture2D>        g_sharedOutTexture;
static ComPtr<ID3D11Fence>            g_sharedOutFence;
static UINT64                         g_sharedOutFrameValue = 0;
static HANDLE                         g_hManifestOut = nullptr;
static BroadcastManifest*             g_pManifestViewOut = nullptr;
static HANDLE                         g_sharedOutTextureHandle = nullptr;
static HANDLE                         g_sharedOutFenceHandle = nullptr;

const char* g_vertexShader = "struct VOut{float4 p:SV_POSITION;float2 u:TEXCOORD;};VOut main(uint v:SV_VertexID){VOut o;o.u=float2((v<<1)&2,v&2);o.p=float4(o.u.x*2-1,1-o.u.y*2,0,1);return o;}";
const char* g_pixelShader = "Texture2D t:register(t0);SamplerState s:register(s0);float4 main(float4 p:SV_POSITION,float2 u:TEXCOORD):SV_TARGET{float4 c=t.Sample(s,u);return float4(1-c.r,1-c.g,c.b,c.a);}";

HRESULT InitD3D()
{
    RETURN_IF_FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &g_device, nullptr, &g_context));
    g_context.As(&g_context4);
    ComPtr<IDXGIDevice> dxgi; g_device.As(&dxgi); ComPtr<IDXGIAdapter> adapter; dxgi->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC desc; adapter->GetDesc(&desc); g_adapterLuid = desc.AdapterLuid;
    return S_OK;
}

HRESULT InitOutputResources()
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 1920; desc.Height = 1080; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    RETURN_IF_FAILED(g_device->CreateTexture2D(&desc, nullptr, &g_outputTexture));
    RETURN_IF_FAILED(g_device->CreateRenderTargetView(g_outputTexture.Get(), nullptr, &g_outputRTV));
    
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    RETURN_IF_FAILED(g_device->CreateTexture2D(&desc, nullptr, &g_sharedOutTexture));
    
    ComPtr<ID3D11Device5> device5; g_device.As(&device5);
    RETURN_IF_FAILED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_sharedOutFence)));
    
    wil::unique_hlocal_security_descriptor sd; PSECURITY_DESCRIPTOR sd_ptr = nullptr;
    THROW_IF_WIN32_BOOL_FALSE(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd_ptr, NULL));
    sd.reset(sd_ptr);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd.get(), FALSE };

    DWORD pid = GetCurrentProcessId();
    std::wstring manifestName = L"DirectPort_Producer_Manifest_" + std::to_wstring(pid);
    std::wstring textureName = L"Global\\DirectPortTexture_" + std::to_wstring(pid);
    std::wstring fenceName = L"Global\\DirectPortFence_" + std::to_wstring(pid);

    ComPtr<IDXGIResource1> r1; g_sharedOutTexture.As(&r1);
    RETURN_IF_FAILED(r1->CreateSharedHandle(&sa, GENERIC_ALL, textureName.c_str(), &g_sharedOutTextureHandle));
    RETURN_IF_FAILED(g_sharedOutFence->CreateSharedHandle(&sa, GENERIC_ALL, fenceName.c_str(), &g_sharedOutFenceHandle));
    
    g_hManifestOut = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(BroadcastManifest), manifestName.c_str());
    RETURN_HR_IF_NULL(E_FAIL, g_hManifestOut);
    g_pManifestViewOut = (BroadcastManifest*)MapViewOfFile(g_hManifestOut, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    RETURN_HR_IF_NULL(E_FAIL, g_pManifestViewOut);
    
    ZeroMemory(g_pManifestViewOut, sizeof(BroadcastManifest));
    g_pManifestViewOut->width = 1920; g_pManifestViewOut->height = 1080; g_pManifestViewOut->format = DXGI_FORMAT_B8G8R8A8_UNORM;
    g_pManifestViewOut->adapterLuid = g_adapterLuid;
    wcscpy_s(g_pManifestViewOut->textureName, textureName.c_str());
    wcscpy_s(g_pManifestViewOut->fenceName, fenceName.c_str());
    
    ComPtr<ID3DBlob> vsBlob, psBlob;
    D3DCompile(g_vertexShader, strlen(g_vertexShader), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, nullptr);
    D3DCompile(g_pixelShader, strlen(g_pixelShader), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, nullptr);
    g_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_vs);
    g_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_ps);
    
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP; sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    g_device->CreateSamplerState(&sampDesc, &g_sampler);

    return S_OK;
}

void FindAndConnectInput()
{
    g_discovery->DiscoverStreams();
    const auto& streams = g_discovery->GetDiscoveredStreams();
    if (streams.empty()) { g_inputConnected = false; return; }
    
    g_inputStream = streams[0];

    ComPtr<ID3D11Device1> d1; g_device.As(&d1);
    ComPtr<ID3D11Device5> d5; g_device.As(&d5);
    wil::unique_handle hTex(GetHandleFromName(g_inputStream.textureName.c_str()));
    wil::unique_handle hFence(GetHandleFromName(g_inputStream.fenceName.c_str()));
    
    if (!hTex || FAILED(d1->OpenSharedResource1(hTex.get(), IID_PPV_ARGS(&g_inputSharedTexture)))) { g_inputConnected = false; return; }
    if (!hFence || FAILED(d5->OpenSharedFence(hFence.get(), IID_PPV_ARGS(&g_inputSharedFence)))) { g_inputConnected = false; return; }
    
    D3D11_TEXTURE2D_DESC desc; g_inputSharedTexture->GetDesc(&desc);
    desc.MiscFlags = 0; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE; desc.Usage = D3D11_USAGE_DEFAULT;
    g_device->CreateTexture2D(&desc, nullptr, &g_inputPrivateTexture);
    g_device->CreateShaderResourceView(g_inputPrivateTexture.Get(), nullptr, &g_inputSRV);
    
    g_inputConnected = true;
}

PRODUCER_API HRESULT InitializeProducer(const wchar_t* args)
{
    RETURN_IF_FAILED(InitD3D());
    RETURN_IF_FAILED(InitOutputResources());
    g_discovery = std::make_unique<VirtuaCam::Discovery>();
    g_discovery->Initialize(g_device.Get());
    return S_OK;
}

PRODUCER_API void ProcessFrame()
{
    if (!g_inputConnected) { FindAndConnectInput(); return; }

    wil::unique_handle hManifest(OpenFileMappingW(FILE_MAP_READ, FALSE, g_inputStream.manifestName.c_str()));
    if(!hManifest) { g_inputConnected = false; return; }
    BroadcastManifest* pView = (BroadcastManifest*)MapViewOfFile(hManifest.get(), FILE_MAP_READ, 0, 0, sizeof(BroadcastManifest));
    if(!pView) { g_inputConnected = false; return; }

    UINT64 latest = pView->frameValue;
    UnmapViewOfFile(pView);
    if(latest > g_lastSeenFrame) {
        g_context4->Wait(g_inputSharedFence.Get(), latest);
        g_context->CopyResource(g_inputPrivateTexture.Get(), g_inputSharedTexture.Get());
        g_lastSeenFrame = latest;
    }
    
    D3D11_VIEWPORT vp = {0,0,1920,1080,0,1};
    g_context->RSSetViewports(1, &vp);
    g_context->OMSetRenderTargets(1, g_outputRTV.GetAddressOf(), nullptr);
    g_context->VSSetShader(g_vs.Get(), nullptr, 0);
    g_context->PSSetShader(g_ps.Get(), nullptr, 0);
    g_context->PSSetSamplers(0, 1, g_sampler.GetAddressOf());
    g_context->PSSetShaderResources(0, 1, g_inputSRV.GetAddressOf());
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->Draw(3, 0);
    
    g_context->CopyResource(g_sharedOutTexture.Get(), g_outputTexture.Get());
    g_sharedOutFrameValue++;
    g_context4->Signal(g_sharedOutFence.Get(), g_sharedOutFrameValue);
    if (g_pManifestViewOut) {
        InterlockedExchange64(reinterpret_cast<volatile LONGLONG*>(&g_pManifestViewOut->frameValue), g_sharedOutFrameValue);
    }
}

PRODUCER_API void ShutdownProducer()
{
    if (g_pManifestViewOut) UnmapViewOfFile(g_pManifestViewOut);
    if (g_hManifestOut) CloseHandle(g_hManifestOut);
    if (g_sharedOutTextureHandle) CloseHandle(g_sharedOutTextureHandle);
    if (g_sharedOutFenceHandle) CloseHandle(g_sharedOutFenceHandle);
}