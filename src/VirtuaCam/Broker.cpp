#define WIN32_LEAN_AND_MEAN
#include "pch.h"
#include <wrl.h>
#include <sddl.h>
#include <string>
#include <vector>
#include <mutex>
#include <algorithm>
#include "wil/resource.h"
#include "App.h"
#include "Tools.h"
#include "Formats.h"
#include "Discovery.h"
#include "Multiplexer.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "advapi32.lib")

using namespace Microsoft::WRL;

#define BROKER_API __declspec(dllexport)

static ComPtr<ID3D11Device> g_device;
static LUID g_adapterLuid = {};

static std::unique_ptr<VirtuaCam::Discovery> g_discovery;
static std::unique_ptr<Multiplexer> g_multiplexer;

const WCHAR* BROKER_MANIFEST_NAME = L"Global\\DirectPort_Producer_Manifest_VirtuaCast_Broker";
static ComPtr<ID3D11Texture2D> g_sharedTex_Out;
static ComPtr<ID3D11Fence> g_sharedFence_Out;
static HANDLE g_hManifest_Out = nullptr;
static BroadcastManifest* g_pManifestView_Out = nullptr;
static HANDLE g_sharedNTHandle_Out = nullptr;
static HANDLE g_sharedFenceHandle_Out = nullptr;

static std::vector<DWORD> g_producerPriorityList;
static std::mutex g_producerListMutex;
static bool g_isGridMode = false;

static BrokerState g_brokerState = BrokerState::Searching;

void ShutdownSharing() {
    if (g_pManifestView_Out) UnmapViewOfFile(g_pManifestView_Out);
    if (g_hManifest_Out) CloseHandle(g_hManifest_Out);
    if (g_sharedNTHandle_Out) CloseHandle(g_sharedNTHandle_Out);
    if (g_sharedFenceHandle_Out) CloseHandle(g_sharedFenceHandle_Out);
    g_pManifestView_Out = nullptr; g_hManifest_Out = nullptr;
    g_sharedNTHandle_Out = nullptr; g_sharedFenceHandle_Out = nullptr;
    g_sharedTex_Out.Reset(); g_sharedFence_Out.Reset();
}

HRESULT CreateSharingResources(UINT width, UINT height, DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width; td.Height = height; td.Format = format; td.MipLevels = 1;
    td.ArraySize = 1; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    RETURN_IF_FAILED(g_device->CreateTexture2D(&td, nullptr, g_sharedTex_Out.GetAddressOf()));
    
    ComPtr<ID3D11Device5> device5;
    g_device.As(&device5);
    RETURN_IF_FAILED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(g_sharedFence_Out.GetAddressOf())));

    wil::unique_hlocal_security_descriptor sd;
    PSECURITY_DESCRIPTOR sd_ptr = nullptr;
    THROW_IF_WIN32_BOOL_FALSE(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd_ptr, NULL));
    sd.reset(sd_ptr);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd.get(), FALSE };

    const wchar_t* textureName = L"Global\\VirtuaCast_Broker_Texture";
    const wchar_t* fenceName = L"Global\\VirtuaCast_Broker_Fence";
    ComPtr<IDXGIResource1> r1;
    g_sharedTex_Out.As(&r1);
    RETURN_IF_FAILED(r1->CreateSharedHandle(&sa, GENERIC_ALL, textureName, &g_sharedNTHandle_Out));
    RETURN_IF_FAILED(g_sharedFence_Out->CreateSharedHandle(&sa, GENERIC_ALL, fenceName, &g_sharedFenceHandle_Out));

    g_hManifest_Out = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(BroadcastManifest), BROKER_MANIFEST_NAME);
    if (!g_hManifest_Out) return HRESULT_FROM_WIN32(GetLastError());
    
    g_pManifestView_Out = (BroadcastManifest*)MapViewOfFile(g_hManifest_Out, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BroadcastManifest));
    if (!g_pManifestView_Out) { ShutdownSharing(); return E_FAIL; }
    
    ZeroMemory(g_pManifestView_Out, sizeof(BroadcastManifest));
    g_pManifestView_Out->width = width; g_pManifestView_Out->height = height;
    g_pManifestView_Out->format = format; g_pManifestView_Out->adapterLuid = g_adapterLuid;
    g_pManifestView_Out->command = VCamCommand::None;
    wcscpy_s(g_pManifestView_Out->textureName, _countof(g_pManifestView_Out->textureName), textureName);
    wcscpy_s(g_pManifestView_Out->fenceName, _countof(g_pManifestView_Out->fenceName), fenceName);
    return S_OK;
}

HRESULT InitD3D11_Broker() {
    ComPtr<ID3D11DeviceContext> context;
    RETURN_IF_FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &g_device, nullptr, &context));
    ComPtr<IDXGIDevice> dxgi; g_device.As(&dxgi); ComPtr<IDXGIAdapter> adapter; dxgi->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC desc; adapter->GetDesc(&desc); g_adapterLuid = desc.AdapterLuid;
    return S_OK;
}

extern "C" {
    BROKER_API void InitializeBroker() {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (SUCCEEDED(InitD3D11_Broker())) {
            g_discovery = std::make_unique<VirtuaCam::Discovery>();
            g_discovery->Initialize(g_device.Get());

            g_multiplexer = std::make_unique<Multiplexer>();
            g_multiplexer->Initialize(g_device);

            CreateSharingResources(1920, 1080, DXGI_FORMAT_B8G8R8A8_UNORM);
        }
    }

    BROKER_API void ShutdownBroker() {
        ShutdownSharing();
        if (g_multiplexer) g_multiplexer->Shutdown();
        if (g_discovery) g_discovery->Teardown();
        g_device.Reset();
        g_multiplexer.reset();
        g_discovery.reset();
        CoUninitialize();
    }
    
    BROKER_API void UpdateProducerPriorityList(const DWORD* pids, int count) {
        std::lock_guard<std::mutex> lock(g_producerListMutex);
        g_producerPriorityList.assign(pids, pids + count);
    }
    
    BROKER_API void SetCompositingMode(bool isGrid) {
        g_isGridMode = isGrid;
    }

    BROKER_API void RenderBrokerFrame() {
        if (!g_discovery || !g_multiplexer || !g_pManifestView_Out) return;
        
        g_discovery->DiscoverStreams();
        const auto& allStreams = g_discovery->GetDiscoveredStreams();
        
        std::vector<VirtuaCam::DiscoveredSharedStream> streamsToMux;
        
        {
            std::lock_guard<std::mutex> lock(g_producerListMutex);
            if (g_isGridMode) {
                streamsToMux = allStreams;
            } else {
                streamsToMux.reserve(g_producerPriorityList.size());
                for (DWORD pid : g_producerPriorityList) {
                    if (pid == 0) {
                        streamsToMux.push_back({});
                    } else {
                        auto it = std::find_if(allStreams.begin(), allStreams.end(), 
                            [pid](const auto& s){ return s.processId == pid; });
                        
                        if (it != allStreams.end()) {
                            streamsToMux.push_back(*it);
                        } else {
                            streamsToMux.push_back({});
                        }
                    }
                }
            }
        }

        if(!streamsToMux.empty() && std::any_of(streamsToMux.begin(), streamsToMux.end(), [](const auto& s){ return s.processId != 0;})) {
            g_brokerState = BrokerState::Connected;
        } else if (!allStreams.empty()) {
             g_brokerState = BrokerState::Searching;
        } else {
            g_brokerState = BrokerState::Failed;
        }
        
        g_multiplexer->CompositeFrames(streamsToMux, g_isGridMode);

        ComPtr<ID3D11DeviceContext> context;
        g_device->GetImmediateContext(&context);
        context->CopyResource(g_sharedTex_Out.Get(), g_multiplexer->GetOutputTexture());
        
        ComPtr<ID3D11DeviceContext4> context4;
        context.As(&context4);
        UINT64 frameValue = g_multiplexer->GetOutputFrameValue();
        context4->Signal(g_sharedFence_Out.Get(), frameValue);
        
        InterlockedExchange64(reinterpret_cast<volatile LONGLONG*>(&g_pManifestView_Out->frameValue), frameValue);
        g_pManifestView_Out->command = VCamCommand::None;
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
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(hModule);
    return TRUE;
}