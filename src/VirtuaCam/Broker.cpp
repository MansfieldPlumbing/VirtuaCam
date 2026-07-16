// =============================================================================
// Broker.cpp  --  GPU compositing broker
// =============================================================================
// The broker is a DLL (DirectPortBroker.dll) loaded by VirtuaCam.exe at
// runtime.  It sits at the centre of VirtuaCam's pipeline:
//
//   [Producer processes]  -->  Broker  -->  [Virtual camera DLL (DirectPortClient)]
//
// Responsibilities:
//   1. Initialise a D3D11 device and record our GPU adapter LUID.
//   2. Create a shared output texture + fence and publish their NT handle names
//      in a named file-mapping (the "broker manifest") so that the virtual
//      camera DLL (BrokerClient) can discover and open them.
//   3. Each frame: run Discovery to find live producers, pass the result to the
//      Multiplexer, copy the composited frame to the shared output texture, and
//      signal the output fence so consumers know a new frame is ready.
//   4. Maintain a producer priority list supplied by the UI, which controls
//      which producer is the primary (fullscreen) source and which appear as
//      picture-in-picture overlays.
//
// KEY DESIGN DECISIONS:
//
// 1. Local\ Namespace (NOT Global\)
//    - Standard users lack SeCreateGlobalPrivilege required for Global\ namespace
//    - Local\ exists within the user session, avoiding ERROR_ACCESS_DENIED
//    - Frame Server (LOCAL SERVICE) can still access Local\ handles via Creator-Consumer pattern
//
// 2. Permissive Security Descriptor: "D:P(A;;GA;;;AU)"
//    - D:P = DACL present (no inheritance)
//    - A = Allow
//    - GA = GENERIC_ALL (full access)
//    - AU = Authenticated Users (SID)
//    This grants the Frame Server process permission to open handles created by
//    the user-mode broker, even across session boundaries.
//
// 3. Creator-Consumer Pattern
//    - Broker (user-mode) creates shared texture/fence with permissive DACL
//    - DirectPortClient.dll loaded by Frame Server (LOCAL SERVICE) opens them
//    - No admin privileges required for the main application executable
// =============================================================================

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

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

static ComPtr<ID3D11Device> g_device;
static LUID g_adapterLuid = {};

static std::unique_ptr<VirtuaCam::Discovery> g_discovery;
static std::unique_ptr<Multiplexer> g_multiplexer;

// Name of the broker's own manifest — BrokerClient opens this to find the
// shared output texture and fence.
const WCHAR* BROKER_MANIFEST_NAME = L"Local\\DirectPort_Producer_Manifest_VirtuaCast_Broker";

// Shared output resources (broker -> BrokerClient / virtual camera consumer)
static ComPtr<ID3D11Texture2D> g_sharedTex_Out;
static ComPtr<ID3D11Fence>     g_sharedFence_Out;
static HANDLE                  g_hManifest_Out         = nullptr;
static BroadcastManifest*      g_pManifestView_Out      = nullptr;
static HANDLE                  g_sharedNTHandle_Out     = nullptr;
static HANDLE                  g_sharedFenceHandle_Out  = nullptr;

// Producer priority list — ordered array of PIDs set by the UI.
// producers[0] is the primary (fullscreen) source; producers[1..3] are PiP.
// A PID of 0 means "off" for that slot.
static std::vector<DWORD> g_producerPriorityList;
static std::mutex         g_producerListMutex;

static bool        g_isGridMode  = false;
static BrokerState g_brokerState = BrokerState::Searching;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void ShutdownSharing() {
    if (g_pManifestView_Out)    UnmapViewOfFile(g_pManifestView_Out);
    if (g_hManifest_Out)        CloseHandle(g_hManifest_Out);
    if (g_sharedNTHandle_Out)   CloseHandle(g_sharedNTHandle_Out);
    if (g_sharedFenceHandle_Out) CloseHandle(g_sharedFenceHandle_Out);
    g_pManifestView_Out      = nullptr;
    g_hManifest_Out          = nullptr;
    g_sharedNTHandle_Out     = nullptr;
    g_sharedFenceHandle_Out  = nullptr;
    g_sharedTex_Out.Reset();
    g_sharedFence_Out.Reset();
}

// Create the shared D3D11 texture, fence, and broker manifest that consumers
// (BrokerClient) open to read composited frames from the broker.
HRESULT CreateSharingResources(UINT width, UINT height, DXGI_FORMAT format) {
    // D3D11_RESOURCE_MISC_SHARED_NTHANDLE is required for named cross-process
    // sharing via CreateSharedHandle / OpenSharedResource1.
    D3D11_TEXTURE2D_DESC td{};
    td.Width       = width;
    td.Height      = height;
    td.Format      = format;
    td.MipLevels   = 1;
    td.ArraySize   = 1;
    td.SampleDesc.Count = 1;
    td.Usage       = D3D11_USAGE_DEFAULT;
    td.BindFlags   = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags   = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    RETURN_IF_FAILED(g_device->CreateTexture2D(&td, nullptr, g_sharedTex_Out.GetAddressOf()));

    // ID3D11Fence (D3D11.4) provides GPU-timeline synchronisation across
    // processes; the consumer waits on this fence before reading the texture.
    ComPtr<ID3D11Device5> device5;
    g_device.As(&device5);
    RETURN_IF_FAILED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(g_sharedFence_Out.GetAddressOf())));

    // =============================================================================
    // SECURITY DESCRIPTOR: "D:P(A;;GA;;;AU)"
    // =============================================================================
    // This is CRITICAL for cross-process access without admin privileges:
    //   - D:P  = DACL present (no inheritance from parent)
    //   - A    = Access Allowed ACE
    //   - GA   = GENERIC_ALL (full access rights)
    //   - AU   = Authenticated Users (built-in SID: S-1-5-11)
    //
    // WHY THIS MATTERS:
    // The Windows Camera Frame Server runs as LOCAL SERVICE in Session 0.
    // When DirectPortClient.dll is loaded by the Frame Server, it needs to open
    // the shared texture and fence handles created by our user-mode broker.
    // Without this permissive DACL, OpenSharedResource1 would fail with
    // ERROR_ACCESS_DENIED when crossing session boundaries.
    // =============================================================================
    wil::unique_hlocal_security_descriptor sd;
    PSECURITY_DESCRIPTOR sd_ptr = nullptr;
    THROW_IF_WIN32_BOOL_FALSE(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd_ptr, NULL));
    sd.reset(sd_ptr);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd.get(), FALSE };

    // =============================================================================
    // LOCAL\\ NAMESPACE (not Global\\)
    // =============================================================================
    // Using Local\\ instead of Global\\ is ESSENTIAL for standard user execution:
    //   - Global\\ requires SeCreateGlobalPrivilege (admin-only by default)
    //   - Local\\ exists within the user session, no special privileges needed
    //   - Frame Server can still access these handles because:
    //     a) Creator-Consumer pattern: DLL loaded by Frame Server opens handles
    //     b) Permissive DACL above grants Authenticated Users full access
    //     c) Local\\ handles are visible within the same logon session
    // =============================================================================
    const wchar_t* textureName = L"Local\\VirtuaCast_Broker_Texture";
    const wchar_t* fenceName   = L"Local\\VirtuaCast_Broker_Fence";

    ComPtr<IDXGIResource1> r1;
    g_sharedTex_Out.As(&r1);
    RETURN_IF_FAILED(r1->CreateSharedHandle(&sa, GENERIC_ALL, textureName, &g_sharedNTHandle_Out));
    RETURN_IF_FAILED(g_sharedFence_Out->CreateSharedHandle(&sa, GENERIC_ALL, fenceName, &g_sharedFenceHandle_Out));

    // Publish the broker manifest (named file-mapping) so BrokerClient can
    // discover the texture/fence names by simply reading this small struct.
    g_hManifest_Out = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(BroadcastManifest), BROKER_MANIFEST_NAME);
    if (!g_hManifest_Out) return HRESULT_FROM_WIN32(GetLastError());

    g_pManifestView_Out = (BroadcastManifest*)MapViewOfFile(g_hManifest_Out, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BroadcastManifest));
    if (!g_pManifestView_Out) { ShutdownSharing(); return E_FAIL; }

    ZeroMemory(g_pManifestView_Out, sizeof(BroadcastManifest));
    g_pManifestView_Out->width       = width;
    g_pManifestView_Out->height      = height;
    g_pManifestView_Out->format      = format;
    g_pManifestView_Out->adapterLuid = g_adapterLuid;
    g_pManifestView_Out->command     = VCamCommand::None;
    wcscpy_s(g_pManifestView_Out->textureName, _countof(g_pManifestView_Out->textureName), textureName);
    wcscpy_s(g_pManifestView_Out->fenceName,   _countof(g_pManifestView_Out->fenceName),   fenceName);
    return S_OK;
}

HRESULT InitD3D11_Broker() {
    ComPtr<ID3D11DeviceContext> context;
    RETURN_IF_FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &g_device, nullptr, &context));
    // Record the adapter LUID — Discovery uses it to filter producers to our GPU.
    ComPtr<IDXGIDevice>  dxgi;    g_device.As(&dxgi);
    ComPtr<IDXGIAdapter> adapter; dxgi->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC desc;       adapter->GetDesc(&desc);
    g_adapterLuid = desc.AdapterLuid;
    return S_OK;
}

// ---------------------------------------------------------------------------
// Public API (called by VirtuaCam.exe via LoadLibrary / GetProcAddress)
// ---------------------------------------------------------------------------

extern "C" {

    // Initialise D3D11, the discovery scanner, the multiplexer, and the shared
    // output resources.  Must be called once before any other Broker function.
    BROKER_API void InitializeBroker() {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (SUCCEEDED(InitD3D11_Broker())) {
            g_discovery = std::make_unique<VirtuaCam::Discovery>();
            g_discovery->Initialize(g_device.Get());

            g_multiplexer = std::make_unique<Multiplexer>();
            g_multiplexer->Initialize(g_device);

            // Fixed 1080p output — matches the virtual camera's advertised format.
            CreateSharingResources(1920, 1080, DXGI_FORMAT_B8G8R8A8_UNORM);
        }
    }

    BROKER_API void ShutdownBroker() {
        ShutdownSharing();
        if (g_multiplexer) g_multiplexer->Shutdown();
        if (g_discovery)   g_discovery->Teardown();
        g_device.Reset();
        g_multiplexer.reset();
        g_discovery.reset();
        CoUninitialize();
    }

    // Update which producers are active and in what order.
    // pids[0] = primary source, pids[1..3] = PiP overlays.  Pass 0 for "off".
    BROKER_API void UpdateProducerPriorityList(const DWORD* pids, int count) {
        std::lock_guard<std::mutex> lock(g_producerListMutex);
        g_producerPriorityList.assign(pids, pids + count);
    }

    // Switch between priority-list mode (primary + PiP) and grid mode (all
    // discovered producers tiled equally).
    BROKER_API void SetCompositingMode(bool isGrid) {
        g_isGridMode = isGrid;
    }

    // Composite one frame and signal the output fence.
    // Called by VirtuaCam.exe's render loop at ~30 fps.
    BROKER_API void RenderBrokerFrame() {
        if (!g_discovery || !g_multiplexer || !g_pManifestView_Out) return;

        // Discover all live producers on this GPU.
        g_discovery->DiscoverStreams();
        const auto& allStreams = g_discovery->GetDiscoveredStreams();

        // Build the ordered list of streams to composite.
        std::vector<VirtuaCam::DiscoveredSharedStream> streamsToMux;

        {
            std::lock_guard<std::mutex> lock(g_producerListMutex);
            if (g_isGridMode) {
                // Grid mode: include every discovered producer regardless of priority.
                streamsToMux = allStreams;
            } else {
                // Priority-list mode: assemble slots in the order specified by the UI.
                // A slot with PID 0 means "off" — insert an empty entry so the
                // multiplexer can render a blank placeholder for that PiP position.
                streamsToMux.reserve(g_producerPriorityList.size());
                for (DWORD pid : g_producerPriorityList) {
                    if (pid == 0) {
                        streamsToMux.push_back({});
                    } else {
                        auto it = std::find_if(allStreams.begin(), allStreams.end(),
                            [pid](const auto& s){ return s.processId == pid; });
                        streamsToMux.push_back(it != allStreams.end() ? *it : VirtuaCam::DiscoveredSharedStream{});
                    }
                }
            }
        }

        // Update broker state for telemetry display in the UI.
        if (!streamsToMux.empty() && std::any_of(streamsToMux.begin(), streamsToMux.end(), [](const auto& s){ return s.processId != 0; })) {
            g_brokerState = BrokerState::Connected;
        } else if (!allStreams.empty()) {
            g_brokerState = BrokerState::Searching;
        } else {
            g_brokerState = BrokerState::Failed;
        }

        // Run GPU compositing.
        g_multiplexer->CompositeFrames(streamsToMux, g_isGridMode);

        // Copy the composited frame from the multiplexer's internal texture to
        // the shared output texture that BrokerClient has opened.
        ComPtr<ID3D11DeviceContext> context;
        g_device->GetImmediateContext(&context);
        context->CopyResource(g_sharedTex_Out.Get(), g_multiplexer->GetOutputTexture());

        // Signal the output fence with the new frame counter value.
        // BrokerClient calls ID3D11DeviceContext4::Wait() on this fence before
        // reading the texture, ensuring it never reads a partially-written frame.
        ComPtr<ID3D11DeviceContext4> context4;
        context.As(&context4);
        UINT64 frameValue = g_multiplexer->GetOutputFrameValue();
        context4->Signal(g_sharedFence_Out.Get(), frameValue);

        // Publish the new frame counter atomically so BrokerClient can detect
        // it without a kernel event.  InterlockedExchange64 prevents torn reads.
        InterlockedExchange64(reinterpret_cast<volatile LONGLONG*>(&g_pManifestView_Out->frameValue), frameValue);
        g_pManifestView_Out->command = VCamCommand::None;
    }

    // Return the shared output texture (with an AddRef).
    // Used by the UI preview window to display the broker's output.
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
