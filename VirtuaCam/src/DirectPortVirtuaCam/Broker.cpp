#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl.h>
#include <sddl.h>
#include <string>
#include <chrono>
#include <vector>
#include <d3dcompiler.h>
#include <d3d12.h>
#include <tlhelp32.h>
#include "wil/resource.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d12.lib")

using namespace Microsoft::WRL;

#define BROKER_API __declspec(dllexport)

void Log(const std::wstring& msg) {
    WCHAR buffer[1024];
    wsprintfW(buffer, L"[PID:%lu][VirtuaCastBroker-Passthrough] %s\n", GetCurrentProcessId(), msg.c_str());
    OutputDebugStringW(buffer);
}

struct BroadcastManifest {
    UINT64 frameValue; UINT width; UINT height; DXGI_FORMAT format;
    LUID adapterLuid; WCHAR textureName[256]; WCHAR fenceName[256];
};

struct ProducerConnection {
    bool isConnected = false;
    DWORD producerPid = 0;
    WCHAR producerName[MAX_PATH] = {};
    HANDLE hManifest = nullptr;
    BroadcastManifest* pManifestView = nullptr;
    ComPtr<ID3D11Texture2D> sharedTexture;
    ComPtr<ID3D11Fence> sharedFence;
    UINT64 lastSeenFrame = 0;
    ComPtr<ID3D11Texture2D> privateTexture;
    ComPtr<ID3D11ShaderResourceView> privateSRV;
};

enum class BrokerState { Searching, Connected, Failed };

static ComPtr<ID3D11Device> g_device;
static ComPtr<ID3D11Device1> g_device1;
static ComPtr<ID3D11Device5> g_device5;
static ComPtr<ID3D11DeviceContext> g_context;
static ComPtr<ID3D11DeviceContext4> g_context4;
static LUID g_adapterLuid = {};

static ComPtr<ID3D11VertexShader> g_blitVS;
static ComPtr<ID3D11PixelShader> g_blitPS;
static ComPtr<ID3D11SamplerState> g_blitSampler;

static ProducerConnection g_inputProducer;
static BrokerState g_brokerState = BrokerState::Searching;
static auto g_lastProducerSearchTime = std::chrono::steady_clock::now();

const WCHAR* BROKER_MANIFEST_NAME = L"Global\\DirectPort_Producer_Manifest_VirtuaCast_Broker";
static ComPtr<ID3D11Texture2D> g_sharedTex_Out;
static ComPtr<ID3D11RenderTargetView> g_sharedTexRTV_Out;
static ComPtr<ID3D11Fence> g_sharedFence_Out;
static UINT64 g_frameValue_Out = 0;
static HANDLE g_hManifest_Out = nullptr;
static BroadcastManifest* g_pManifestView_Out = nullptr;
static HANDLE g_sharedNTHandle_Out = nullptr;
static HANDLE g_sharedFenceHandle_Out = nullptr;

const char* g_BlitVertexShader = R"(
struct VS_OUTPUT {
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD;
};
VS_OUTPUT main(uint id : SV_VertexID) {
    VS_OUTPUT output;
    output.Tex = float2((id << 1) & 2, id & 2);
    output.Pos = float4(output.Tex.x * 2.0 - 1.0, 1.0 - output.Tex.y * 2.0, 0, 1);
    return output;
})";

const char* g_BlitPixelShader = R"(
Texture2D    g_texture : register(t0);
SamplerState g_sampler : register(s0);
struct VS_OUTPUT {
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD;
};
float4 main(VS_OUTPUT input) : SV_TARGET {
    return g_texture.Sample(g_sampler, input.Tex);
})";

void DisconnectFromProducer();

HANDLE GetHandleFromName(const WCHAR* name) {
    ComPtr<ID3D12Device> d3d12Device;
    D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12Device));
    if (!d3d12Device) return NULL;
    HANDLE handle = nullptr;
    d3d12Device->OpenSharedHandleByName(name, GENERIC_ALL, &handle);
    return handle;
}

void ShutdownSharing() {
    if (g_pManifestView_Out) UnmapViewOfFile(g_pManifestView_Out);
    if (g_hManifest_Out) CloseHandle(g_hManifest_Out);
    if (g_sharedNTHandle_Out) CloseHandle(g_sharedNTHandle_Out);
    if (g_sharedFenceHandle_Out) CloseHandle(g_sharedFenceHandle_Out);
    g_pManifestView_Out = nullptr; g_hManifest_Out = nullptr;
    g_sharedNTHandle_Out = nullptr; g_sharedFenceHandle_Out = nullptr;
    g_sharedTex_Out.Reset(); g_sharedFence_Out.Reset(); g_sharedTexRTV_Out.Reset();
    Log(L"Broker output stream has been shut down.");
}

HRESULT CreateSharingResources(UINT width, UINT height, DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width; td.Height = height; td.Format = format; td.MipLevels = 1;
    td.ArraySize = 1; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    HRESULT hr = g_device->CreateTexture2D(&td, nullptr, &g_sharedTex_Out);
    if (FAILED(hr)) { Log(L"Failed to create shared texture"); return hr; }

    hr = g_device->CreateRenderTargetView(g_sharedTex_Out.Get(), nullptr, &g_sharedTexRTV_Out);
    if (FAILED(hr)) { Log(L"Failed to create RTV for shared texture"); return hr; }

    hr = g_device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_sharedFence_Out));
    if (FAILED(hr)) { Log(L"Failed to create shared fence"); return hr; }

    PSECURITY_DESCRIPTOR sd = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;WD)(A;;GA;;;AC)", SDDL_REVISION_1, &sd, NULL);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd, FALSE };

    const wchar_t* textureName = L"Global\\VirtuaCast_Broker_Texture";
    const wchar_t* fenceName = L"Global\\VirtuaCast_Broker_Fence";
    ComPtr<IDXGIResource1> r1;
    g_sharedTex_Out.As(&r1);
    hr = r1->CreateSharedHandle(&sa, GENERIC_ALL, textureName, &g_sharedNTHandle_Out);
    if (FAILED(hr)) { LocalFree(sd); return hr; }
    hr = g_sharedFence_Out->CreateSharedHandle(&sa, GENERIC_ALL, fenceName, &g_sharedFenceHandle_Out);
    if (FAILED(hr)) { LocalFree(sd); return hr; }

    g_hManifest_Out = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(BroadcastManifest), BROKER_MANIFEST_NAME);
    if (!g_hManifest_Out) { LocalFree(sd); return E_FAIL; }
    
    g_pManifestView_Out = (BroadcastManifest*)MapViewOfFile(g_hManifest_Out, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!g_pManifestView_Out) { ShutdownSharing(); LocalFree(sd); return E_FAIL; }
    
    LocalFree(sd);

    ZeroMemory(g_pManifestView_Out, sizeof(BroadcastManifest));
    g_pManifestView_Out->width = width; g_pManifestView_Out->height = height;
    g_pManifestView_Out->format = format; g_pManifestView_Out->adapterLuid = g_adapterLuid;
    wcscpy_s(g_pManifestView_Out->textureName, _countof(g_pManifestView_Out->textureName), textureName);
    wcscpy_s(g_pManifestView_Out->fenceName, _countof(g_pManifestView_Out->fenceName), fenceName);
    Log(L"Broker output stream created successfully.");
    return S_OK;
}

HRESULT InitD3D11_Broker() {
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &g_device, nullptr, &g_context);
    if (FAILED(hr)) return hr;
    if (FAILED(g_device.As(&g_device1)) || FAILED(g_device.As(&g_device5)) || FAILED(g_context.As(&g_context4))) return E_NOINTERFACE;
    ComPtr<IDXGIDevice> dxgi; g_device.As(&dxgi); ComPtr<IDXGIAdapter> adapter; dxgi->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC desc; adapter->GetDesc(&desc); g_adapterLuid = desc.AdapterLuid;
    return S_OK;
}

HRESULT CreateBlitResources() {
    if (g_blitVS && g_blitPS && g_blitSampler) return S_OK;
    
    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> psBlob;
    HRESULT hr = D3DCompile(g_BlitVertexShader, strlen(g_BlitVertexShader), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, nullptr);
    if (FAILED(hr)) { Log(L"Failed to compile vertex shader."); return hr; }
    
    hr = D3DCompile(g_BlitPixelShader, strlen(g_BlitPixelShader), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, nullptr);
    if (FAILED(hr)) { Log(L"Failed to compile pixel shader."); return hr; }

    hr = g_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_blitVS);
    if (FAILED(hr)) { Log(L"Failed to create vertex shader."); return hr; }
    
    hr = g_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_blitPS);
    if (FAILED(hr)) { Log(L"Failed to create pixel shader."); return hr; }
    
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = g_device->CreateSamplerState(&sampDesc, &g_blitSampler);
    if(FAILED(hr)) { Log(L"Failed to create sampler state."); return hr; }

    Log(L"Blit resources created successfully.");
    return S_OK;
}

void FindAndConnectToProducer() {
    if (g_inputProducer.isConnected) {
        HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, g_inputProducer.producerPid);
        if (hProcess == NULL || WaitForSingleObject(hProcess, 0) != WAIT_TIMEOUT) {
            Log(L"Upstream producer process has exited. Disconnecting.");
            DisconnectFromProducer();
        }
        if (hProcess) CloseHandle(hProcess);
        return;
    }

    auto now = std::chrono::steady_clock::now();
    if (now - g_lastProducerSearchTime < std::chrono::seconds(2)) {
        return;
    }
    g_lastProducerSearchTime = now;
    g_brokerState = BrokerState::Searching;

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return;
    
    PROCESSENTRY32W pe32 = {};
    pe32.dwSize = sizeof(PROCESSENTRY32W);
    DWORD selfPid = GetCurrentProcessId();

    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            if (pe32.th32ProcessID == selfPid) continue;

            const std::vector<std::wstring> producerSignatures = { L"DirectPort_Producer_Manifest_" };
            HANDLE hManifest = nullptr;
            for (const auto& sig : producerSignatures) {
                 std::wstring manifestName = sig + std::to_wstring(pe32.th32ProcessID);
                 hManifest = OpenFileMappingW(FILE_MAP_READ, FALSE, manifestName.c_str());
                 if (hManifest) break;
            }
            if (!hManifest) continue;

            BroadcastManifest* pManifestView = (BroadcastManifest*)MapViewOfFile(hManifest, FILE_MAP_READ, 0, 0, sizeof(BroadcastManifest));
            if (!pManifestView) { CloseHandle(hManifest); continue; }

            if (memcmp(&pManifestView->adapterLuid, &g_adapterLuid, sizeof(LUID)) != 0) {
                UnmapViewOfFile(pManifestView); CloseHandle(hManifest); continue;
            }

            ComPtr<ID3D11Texture2D> tempTexture;
            ComPtr<ID3D11Fence> tempFence;
            {
                wil::unique_handle hTexture(GetHandleFromName(pManifestView->textureName));
                wil::unique_handle hFence(GetHandleFromName(pManifestView->fenceName));

                if (!hTexture || FAILED(g_device1->OpenSharedResource1(hTexture.get(), IID_PPV_ARGS(&tempTexture)))) {
                    UnmapViewOfFile(pManifestView); CloseHandle(hManifest); continue;
                }
                if (!hFence || FAILED(g_device5->OpenSharedFence(hFence.get(), IID_PPV_ARGS(&tempFence)))) {
                    UnmapViewOfFile(pManifestView); CloseHandle(hManifest); continue;
                }
            }

            Log(L"Connected to upstream producer PID: " + std::to_wstring(pe32.th32ProcessID));
            g_inputProducer.producerPid = pe32.th32ProcessID;
            wcscpy_s(g_inputProducer.producerName, _countof(g_inputProducer.producerName), pe32.szExeFile);
            g_inputProducer.hManifest = hManifest;
            g_inputProducer.pManifestView = pManifestView;
            g_inputProducer.sharedTexture = tempTexture;
            g_inputProducer.sharedFence = tempFence;
            
            D3D11_TEXTURE2D_DESC sharedDesc;
            tempTexture->GetDesc(&sharedDesc);
            sharedDesc.MiscFlags = 0;
            sharedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            sharedDesc.Usage = D3D11_USAGE_DEFAULT;
            g_device->CreateTexture2D(&sharedDesc, nullptr, &g_inputProducer.privateTexture);
            g_device->CreateShaderResourceView(g_inputProducer.privateTexture.Get(), nullptr, &g_inputProducer.privateSRV);

            g_inputProducer.isConnected = true;
            g_brokerState = BrokerState::Connected;
            CloseHandle(hSnapshot);
            return;

        } while (Process32NextW(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
    g_brokerState = BrokerState::Failed;
}

void DisconnectFromProducer() {
    if (!g_inputProducer.isConnected) return;
    if (g_inputProducer.pManifestView) UnmapViewOfFile(g_inputProducer.pManifestView);
    if (g_inputProducer.hManifest) CloseHandle(g_inputProducer.hManifest);
    g_inputProducer = {};
    g_brokerState = BrokerState::Searching;
    Log(L"Disconnected from upstream producer.");
}

extern "C" {
    BROKER_API void InitializeBroker() {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (SUCCEEDED(InitD3D11_Broker()) && 
            SUCCEEDED(CreateSharingResources(1280, 720, DXGI_FORMAT_B8G8R8A8_UNORM)) &&
            SUCCEEDED(CreateBlitResources())) {
            Log(L"Broker initialized successfully.");
        } else {
            Log(L"Broker initialization FAILED.");
        }
    }

    BROKER_API void ShutdownBroker() {
        Log(L"Broker shutting down.");
        DisconnectFromProducer();
        ShutdownSharing();
        g_device.Reset(); g_device1.Reset(); g_device5.Reset(); g_context.Reset(); g_context4.Reset();
        g_blitVS.Reset(); g_blitPS.Reset(); g_blitSampler.Reset();
        CoUninitialize();
    }

    BROKER_API void RenderBrokerFrame() {
        if (!g_sharedTex_Out || !g_context || !g_sharedTexRTV_Out) return;
        
        FindAndConnectToProducer();
        
        if (g_inputProducer.isConnected) {
            UINT64 latestFrame = g_inputProducer.pManifestView->frameValue;
            if (latestFrame > g_inputProducer.lastSeenFrame) {
                g_context4->Wait(g_inputProducer.sharedFence.Get(), latestFrame);
                g_context->CopyResource(g_inputProducer.privateTexture.Get(), g_inputProducer.sharedTexture.Get());
                g_inputProducer.lastSeenFrame = latestFrame;
            }

            D3D11_TEXTURE2D_DESC outDesc;
            g_sharedTex_Out->GetDesc(&outDesc);
            D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)outDesc.Width, (float)outDesc.Height, 0.0f, 1.0f };
            g_context->RSSetViewports(1, &vp);
            g_context->OMSetRenderTargets(1, g_sharedTexRTV_Out.GetAddressOf(), nullptr);

            g_context->VSSetShader(g_blitVS.Get(), nullptr, 0);
            g_context->PSSetShader(g_blitPS.Get(), nullptr, 0);
            g_context->PSSetShaderResources(0, 1, g_inputProducer.privateSRV.GetAddressOf());
            g_context->PSSetSamplers(0, 1, g_blitSampler.GetAddressOf());
            g_context->IASetInputLayout(nullptr);
            g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_context->Draw(3, 0);
        } else {
            const float blueColor[] = { 0.0f, 0.1f, 0.8f, 1.0f };
            g_context->ClearRenderTargetView(g_sharedTexRTV_Out.Get(), blueColor);
        }
        
        g_frameValue_Out++;
        g_context4->Signal(g_sharedFence_Out.Get(), g_frameValue_Out);
        if (g_pManifestView_Out) {
            InterlockedExchange64(reinterpret_cast<volatile LONGLONG*>(&g_pManifestView_Out->frameValue), g_frameValue_Out);
        }
    }

    BROKER_API ID3D11Texture2D* GetSharedTexture() {
        if (g_sharedTex_Out) {
            g_sharedTex_Out->AddRef();
            return g_sharedTex_Out.Get();
        }
        return nullptr;
    }

    BROKER_API BrokerState GetBrokerState() {
        return g_brokerState;
    }

    BROKER_API bool GetProducerInfo(DWORD* pid, WCHAR* nameBuffer, UINT bufferSize)
    {
        if (g_inputProducer.isConnected && pid != nullptr && nameBuffer != nullptr && bufferSize > 0)
        {
            *pid = g_inputProducer.producerPid;
            wcscpy_s(nameBuffer, bufferSize, g_inputProducer.producerName);
            return true;
        }
        if (pid) *pid = 0;
        if (nameBuffer && bufferSize > 0) nameBuffer[0] = L'\0';
        return false;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(hModule);
    return TRUE;
}