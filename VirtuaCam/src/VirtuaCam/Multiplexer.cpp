#include "pch.h"
#include "Multiplexer.h"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <tlhelp32.h>
#include <sddl.h>
#include <cmath>
#include <algorithm>
#include <DirectXMath.h>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "advapi32.lib")

using namespace Microsoft::WRL;
using namespace DirectX;

struct ControlsCB
{
    XMFLOAT4 ProcAmp;
};

struct Multiplexer::Impl
{
    static const int MAX_PRODUCERS = 256;

    ComPtr<ID3D11Device>           d3d11Device;
    ComPtr<ID3D11Device1>          d3d11Device1;
    ComPtr<ID3D11Device5>          d3d11Device5;
    ComPtr<ID3D11DeviceContext>    d3d11Context;
    ComPtr<ID3D11DeviceContext4>   d3d11Context4;
    LUID                           adapterLuid = {};

    UINT                           muxWidth = 1920;
    UINT                           muxHeight = 1080;
    ComPtr<ID3D11Texture2D>        compositeTexture;
    ComPtr<ID3D11RenderTargetView> compositeRTV;
    ComPtr<ID3D11ShaderResourceView> compositeSRV;

    ComPtr<ID3D11Texture2D>        sharedOutTexture;
    ComPtr<ID3D11Fence>            sharedOutFence;
    UINT64                         sharedOutFrameValue = 0;
    HANDLE                         hManifestOut = nullptr;
    BroadcastManifest*             pManifestViewOut = nullptr;
    HANDLE                         sharedOutTextureHandle = nullptr;
    HANDLE                         sharedOutFenceHandle = nullptr;

    ComPtr<ID3D11VertexShader>     vertexShader;
    ComPtr<ID3D11PixelShader>      pixelShader;
    ComPtr<ID3D11SamplerState>     samplerState;
    ComPtr<ID3D11Buffer>           controlsCB;

    ProducerConnection             producers[MAX_PRODUCERS];
    
    DWORD                          preferredPID = 0;
    DWORD                          pipPID = 0;
    
    HANDLE                         hControlsMapping = nullptr;
    VirtuaCamControls*             pControlsView = nullptr;
    VirtuaCamControls              localControls;

    void DisconnectFromProducer(int producerIndex);
    void CleanupSharing();
    HRESULT InitializeSharingResources();
    HRESULT LoadAssets();
};

Multiplexer::Multiplexer() : pImpl(std::make_unique<Impl>()) {}
Multiplexer::~Multiplexer() { Shutdown(); }

HANDLE Multiplexer::GetSharedOutputHandle()
{
    return pImpl->sharedOutTextureHandle;
}

HRESULT Multiplexer::Initialize(ID3D11Device* device)
{
    pImpl->d3d11Device = device;
    RETURN_IF_FAILED(pImpl->d3d11Device.As(&pImpl->d3d11Device1));
    RETURN_IF_FAILED(pImpl->d3d11Device.As(&pImpl->d3d11Device5));
    pImpl->d3d11Device->GetImmediateContext(&pImpl->d3d11Context);
    RETURN_IF_FAILED(pImpl->d3d11Context.As(&pImpl->d3d11Context4));

    ComPtr<IDXGIDevice> dxgiDevice;
    RETURN_IF_FAILED(pImpl->d3d11Device.As(&dxgiDevice));
    ComPtr<IDXGIAdapter> adapter;
    RETURN_IF_FAILED(dxgiDevice->GetAdapter(&adapter));
    DXGI_ADAPTER_DESC desc;
    RETURN_IF_FAILED(adapter->GetDesc(&desc));
    pImpl->adapterLuid = desc.AdapterLuid;

    pImpl->hControlsMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, VIRTUCAM_CONTROLS_SHM_NAME);
    if (pImpl->hControlsMapping)
    {
        pImpl->pControlsView = (VirtuaCamControls*)MapViewOfFile(pImpl->hControlsMapping, FILE_MAP_READ, 0, 0, sizeof(VirtuaCamControls));
    }

    RETURN_IF_FAILED(pImpl->LoadAssets());
    RETURN_IF_FAILED(pImpl->InitializeSharingResources());

    return S_OK;
}

void Multiplexer::Shutdown()
{
    if (pImpl->pControlsView) UnmapViewOfFile(pImpl->pControlsView);
    if (pImpl->hControlsMapping) CloseHandle(pImpl->hControlsMapping);
    pImpl->pControlsView = nullptr;
    pImpl->hControlsMapping = nullptr;

    for (int i = 0; i < Impl::MAX_PRODUCERS; ++i)
    {
        pImpl->DisconnectFromProducer(i);
    }
    pImpl->CleanupSharing();
}

void Multiplexer::Impl::DisconnectFromProducer(int producerIndex)
{
    auto& producer = producers[producerIndex];
    if (!producer.isConnected) return;

    producer.isConnected = false;
    producer.producerPid = 0;
    if (producer.pManifestView) UnmapViewOfFile(producer.pManifestView);
    if (producer.hManifest) CloseHandle(producer.hManifest);
    producer.pManifestView = nullptr;
    producer.hManifest = nullptr;
    producer.sharedTexture.Reset();
    producer.sharedFence.Reset();
    producer.privateTexture.Reset();
    producer.privateSRV.Reset();
}

void Multiplexer::DiscoverAndManageConnections()
{
    static auto lastSearchTime = std::chrono::steady_clock::now() - std::chrono::seconds(2);

    for (int i = 0; i < Impl::MAX_PRODUCERS; ++i) {
        auto& producer = pImpl->producers[i];
        if (!producer.isConnected) continue;
        HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, producer.producerPid);
        if (hProcess == NULL || WaitForSingleObject(hProcess, 0) != WAIT_TIMEOUT) {
            pImpl->DisconnectFromProducer(i);
        }
        if (hProcess) CloseHandle(hProcess);
    }

    if (std::chrono::steady_clock::now() - lastSearchTime < std::chrono::seconds(1)) return;
    lastSearchTime = std::chrono::steady_clock::now();

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe32 = {}; pe32.dwSize = sizeof(PROCESSENTRY32W);
    DWORD selfPid = GetCurrentProcessId();
    const std::wstring producerSignature = L"DirectPort_Producer_Manifest_";

    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            if (pe32.th32ProcessID == selfPid) continue;

            bool alreadyConnected = false;
            for (int i = 0; i < Impl::MAX_PRODUCERS; ++i) {
                if (pImpl->producers[i].isConnected && pImpl->producers[i].producerPid == pe32.th32ProcessID) {
                    alreadyConnected = true;
                    break;
                }
            }
            if (alreadyConnected) continue;

            int availableSlot = -1;
            for (int i = 0; i < Impl::MAX_PRODUCERS; ++i) if (!pImpl->producers[i].isConnected) { availableSlot = i; break; }
            if (availableSlot == -1) { break; }

            std::wstring manifestName = producerSignature + std::to_wstring(pe32.th32ProcessID);
            HANDLE hManifest = OpenFileMappingW(FILE_MAP_READ, FALSE, manifestName.c_str());
            if (!hManifest) continue;

            BroadcastManifest* pManifestView = (BroadcastManifest*)MapViewOfFile(hManifest, FILE_MAP_READ, 0, 0, sizeof(BroadcastManifest));
            if (!pManifestView) { CloseHandle(hManifest); continue; }

            if (memcmp(&pManifestView->adapterLuid, &pImpl->adapterLuid, sizeof(LUID)) != 0) {
                UnmapViewOfFile(pManifestView); CloseHandle(hManifest); continue;
            }

            auto& producer = pImpl->producers[availableSlot];

            HANDLE hFence = VirtuaCam::Utils::Win32::GetHandleFromObjectName(pManifestView->fenceName);
            if (!hFence || FAILED(pImpl->d3d11Device5->OpenSharedFence(hFence, IID_PPV_ARGS(&producer.sharedFence)))) {
                if (hFence) CloseHandle(hFence); UnmapViewOfFile(pManifestView); CloseHandle(hManifest); continue;
            }
            CloseHandle(hFence);

            HANDLE hTexture = VirtuaCam::Utils::Win32::GetHandleFromObjectName(pManifestView->textureName);
            if (!hTexture || FAILED(pImpl->d3d11Device1->OpenSharedResource1(hTexture, IID_PPV_ARGS(&producer.sharedTexture)))) {
                if (hTexture) CloseHandle(hTexture); UnmapViewOfFile(pManifestView); CloseHandle(hManifest); continue;
            }
            CloseHandle(hTexture);

            D3D11_TEXTURE2D_DESC sharedDesc;
            producer.sharedTexture->GetDesc(&sharedDesc);
            sharedDesc.MiscFlags = 0;
            sharedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            sharedDesc.Usage = D3D11_USAGE_DEFAULT;
            pImpl->d3d11Device->CreateTexture2D(&sharedDesc, nullptr, &producer.privateTexture);
            pImpl->d3d11Device->CreateShaderResourceView(producer.privateTexture.Get(), nullptr, &producer.privateSRV);

            producer.producerPid = pe32.th32ProcessID;
            producer.hManifest = hManifest;
            producer.pManifestView = pManifestView;
            producer.isConnected = true;

        } while (Process32NextW(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
}

HRESULT Multiplexer::Composite()
{
    if (pImpl->pControlsView)
    {
        pImpl->localControls = *pImpl->pControlsView;
    }
    
    ControlsCB cbData;
    cbData.ProcAmp.x = pImpl->localControls.brightness / 100.0f; 
    cbData.ProcAmp.y = pImpl->localControls.contrast / 100.0f;   
    cbData.ProcAmp.z = pImpl->localControls.saturation / 100.0f; 
    cbData.ProcAmp.w = 1.0f;
    pImpl->d3d11Context->UpdateSubresource(pImpl->controlsCB.Get(), 0, nullptr, &cbData, 0, 0);

    for (int i = 0; i < Impl::MAX_PRODUCERS; ++i) {
        auto& producer = pImpl->producers[i];
        if (producer.isConnected && producer.pManifestView) {
            UINT64 latestFrame = producer.pManifestView->frameValue;
            if (latestFrame > producer.lastSeenFrame) {
                pImpl->d3d11Context4->Wait(producer.sharedFence.Get(), latestFrame);
                pImpl->d3d11Context->CopyResource(producer.privateTexture.Get(), producer.sharedTexture.Get());
                producer.lastSeenFrame = latestFrame;
            }
        }
    }

    pImpl->d3d11Context->OMSetRenderTargets(1, pImpl->compositeRTV.GetAddressOf(), nullptr);
    const float compositeClearColor[] = { 0.0f, 0.1f, 0.2f, 1.0f };
    pImpl->d3d11Context->ClearRenderTargetView(pImpl->compositeRTV.Get(), compositeClearColor);

    pImpl->d3d11Context->VSSetShader(pImpl->vertexShader.Get(), nullptr, 0);
    pImpl->d3d11Context->PSSetShader(pImpl->pixelShader.Get(), nullptr, 0);
    pImpl->d3d11Context->PSSetSamplers(0, 1, pImpl->samplerState.GetAddressOf());
    pImpl->d3d11Context->PSSetConstantBuffers(0, 1, pImpl->controlsCB.GetAddressOf());
    pImpl->d3d11Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    int mainIndex = -1, pipIndex = -1;
    for (int i = 0; i < Impl::MAX_PRODUCERS; ++i) {
        if (pImpl->producers[i].isConnected && pImpl->producers[i].producerPid == pImpl->preferredPID) mainIndex = i;
        if (pImpl->producers[i].isConnected && pImpl->producers[i].producerPid == pImpl->pipPID) pipIndex = i;
    }

    if (mainIndex != -1) {
        D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)pImpl->muxWidth, (float)pImpl->muxHeight, 0.0f, 1.0f };
        pImpl->d3d11Context->RSSetViewports(1, &vp);
        pImpl->d3d11Context->PSSetShaderResources(0, 1, pImpl->producers[mainIndex].privateSRV.GetAddressOf());
        pImpl->d3d11Context->Draw(3, 0);
    }
    
    if (pipIndex != -1 && pipIndex != mainIndex) {
        float pipWidth = (float)pImpl->muxWidth / 4.0f;
        float pipHeight = (float)pImpl->muxHeight / 4.0f;
        D3D11_VIEWPORT vp = { (float)pImpl->muxWidth - pipWidth - 10.0f, (float)pImpl->muxHeight - pipHeight - 10.0f, pipWidth, pipHeight, 0.0f, 1.0f };
        pImpl->d3d11Context->RSSetViewports(1, &vp);
        pImpl->d3d11Context->PSSetShaderResources(0, 1, pImpl->producers[pipIndex].privateSRV.GetAddressOf());
        pImpl->d3d11Context->Draw(3, 0);
    }

    pImpl->d3d11Context->CopyResource(pImpl->sharedOutTexture.Get(), pImpl->compositeTexture.Get());
    pImpl->sharedOutFrameValue++;
    pImpl->d3d11Context4->Signal(pImpl->sharedOutFence.Get(), pImpl->sharedOutFrameValue);
    if (pImpl->pManifestViewOut) {
        InterlockedExchange64(reinterpret_cast<volatile LONGLONG*>(&pImpl->pManifestViewOut->frameValue), pImpl->sharedOutFrameValue);
    }
    return (mainIndex != -1) ? S_OK : S_FALSE;
}

void Multiplexer::UpdateProducerPriorityList(const DWORD*, int)
{
}

void Multiplexer::SetPreferredProducerPID(DWORD pid)
{
    pImpl->preferredPID = pid;
}

void Multiplexer::SetPipProducerPID(DWORD pid)
{
    pImpl->pipPID = pid;
}

ID3D11ShaderResourceView* Multiplexer::GetOutputSRV()
{
    return pImpl->compositeSRV.Get();
}

HRESULT Multiplexer::Impl::InitializeSharingResources()
{
    D3D11_TEXTURE2D_DESC compositeDesc = {};
    compositeDesc.Width = muxWidth;
    compositeDesc.Height = muxHeight;
    compositeDesc.MipLevels = 1;
    compositeDesc.ArraySize = 1;
    compositeDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    compositeDesc.SampleDesc.Count = 1;
    compositeDesc.Usage = D3D11_USAGE_DEFAULT;
    compositeDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    RETURN_IF_FAILED(d3d11Device->CreateTexture2D(&compositeDesc, nullptr, &compositeTexture));
    RETURN_IF_FAILED(d3d11Device->CreateRenderTargetView(compositeTexture.Get(), nullptr, &compositeRTV));
    RETURN_IF_FAILED(d3d11Device->CreateShaderResourceView(compositeTexture.Get(), nullptr, &compositeSRV));

    D3D11_TEXTURE2D_DESC sharedDesc = compositeDesc;
    sharedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sharedDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    RETURN_IF_FAILED(d3d11Device->CreateTexture2D(&sharedDesc, nullptr, &sharedOutTexture));

    RETURN_IF_FAILED(d3d11Device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&sharedOutFence)));

    DWORD pid = GetCurrentProcessId();
    std::wstring manifestName = L"Global\\DirectPort_Producer_Manifest_VirtuaCast_Broker";
    std::wstring textureName = L"Global\\DirectPortTexture_Broker_" + std::to_wstring(pid);
    std::wstring fenceName = L"Global\\DirectPortFence_Broker_" + std::to_wstring(pid);

    wil::unique_hlocal_security_descriptor sd;
    PSECURITY_DESCRIPTOR sd_ptr = nullptr;
    THROW_IF_WIN32_BOOL_FALSE(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd_ptr, NULL));
    sd.reset(sd_ptr);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd.get(), FALSE };

    ComPtr<IDXGIResource1> resource1;
    RETURN_IF_FAILED(sharedOutTexture.As(&resource1));

    RETURN_IF_FAILED(resource1->CreateSharedHandle(&sa, GENERIC_ALL, textureName.c_str(), &sharedOutTextureHandle));
    RETURN_IF_FAILED(sharedOutFence->CreateSharedHandle(&sa, GENERIC_ALL, fenceName.c_str(), &sharedOutFenceHandle));

    hManifestOut = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(BroadcastManifest), manifestName.c_str());
    RETURN_HR_IF_NULL(E_FAIL, hManifestOut);

    pManifestViewOut = (BroadcastManifest*)MapViewOfFile(hManifestOut, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BroadcastManifest));
    RETURN_HR_IF_NULL(E_FAIL, pManifestViewOut);

    ZeroMemory(pManifestViewOut, sizeof(BroadcastManifest));
    pManifestViewOut->width = muxWidth;
    pManifestViewOut->height = muxHeight;
    pManifestViewOut->format = DXGI_FORMAT_B8G8R8A8_UNORM;
    pManifestViewOut->adapterLuid = adapterLuid;
    wcscpy_s(pManifestViewOut->textureName, textureName.c_str());
    wcscpy_s(pManifestViewOut->fenceName, fenceName.c_str());

    return S_OK;
}

void Multiplexer::Impl::CleanupSharing()
{
    if (pManifestViewOut) UnmapViewOfFile(pManifestViewOut);
    if (hManifestOut) CloseHandle(hManifestOut);
    if (sharedOutTextureHandle) CloseHandle(sharedOutTextureHandle);
    if (sharedOutFenceHandle) CloseHandle(sharedOutFenceHandle);
    pManifestViewOut = nullptr;
    hManifestOut = nullptr;
    sharedOutTextureHandle = nullptr;
    sharedOutFenceHandle = nullptr;
    sharedOutTexture.Reset();
    sharedOutFence.Reset();
    compositeSRV.Reset();
    compositeRTV.Reset();
    compositeTexture.Reset();
}

HRESULT Multiplexer::Impl::LoadAssets()
{
    const char* vertexShaderHLSL = R"(
    struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
    PSInput VSMain(uint id : SV_VertexID) {
        PSInput result;
        float2 uv = float2((id << 1) & 2, id & 2);
        result.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1);
        result.uv = uv;
        return result;
    })";
    const char* pixelShaderHLSL = R"(
    Texture2D g_texture : register(t0);
    SamplerState g_sampler : register(s0);

    cbuffer ControlsCB : register(b0)
    {
        float4 ProcAmp; // x: brightness, y: contrast, z: saturation
    };
    
    struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };

    float4 PSMain(PSInput input) : SV_TARGET {
        float4 color = g_texture.Sample(g_sampler, input.uv);
        
        // Apply contrast
        color.rgb = (color.rgb - 0.5f) * ProcAmp.y + 0.5f;
        
        // Apply brightness
        color.rgb += ProcAmp.x;

        // Apply saturation
        float luma = dot(color.rgb, float3(0.299, 0.587, 0.114));
        color.rgb = lerp(float3(luma, luma, luma), color.rgb, ProcAmp.z);

        return saturate(color);
    })";

    ComPtr<ID3DBlob> vsBlob, psBlob;
    RETURN_IF_FAILED(D3DCompile(vertexShaderHLSL, strlen(vertexShaderHLSL), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsBlob, nullptr));
    RETURN_IF_FAILED(D3DCompile(pixelShaderHLSL, strlen(pixelShaderHLSL), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psBlob, nullptr));

    RETURN_IF_FAILED(d3d11Device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vertexShader));
    RETURN_IF_FAILED(d3d11Device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &pixelShader));

    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    RETURN_IF_FAILED(d3d11Device->CreateSamplerState(&sampDesc, &samplerState));

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(ControlsCB);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    RETURN_IF_FAILED(d3d11Device->CreateBuffer(&cbDesc, nullptr, &controlsCB));

    return S_OK;
}