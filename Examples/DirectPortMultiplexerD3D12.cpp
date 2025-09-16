// --- DirectPortMultiplexerD3D12.cpp ---
// A D3D12-based consumer/producer hybrid that composites multiple streams.
// 1. Automatically discovers and connects to all available producers.
// 2. Renders all consumed streams into a grid layout on a single texture.
// 3. Produces this final composited texture as a new, shareable stream.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl.h>
#include <sddl.h>
#include <string>
#include <chrono>
#include <vector>
#include <cmath>
#include <tlhelp32.h>
#include <algorithm>
#include "resource.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "Synchronization.lib")

using namespace Microsoft::WRL;

// --- Logging & Structs ---
void Log(const std::wstring& msg) { WCHAR b[512]; wsprintfW(b, L"[PID:%lu][MuxD3D12] %s\n", GetCurrentProcessId(), msg.c_str()); OutputDebugStringW(b); }

struct BroadcastManifest {
    UINT64 frameValue; UINT width; UINT height; DXGI_FORMAT format;
    LUID adapterLuid; WCHAR textureName[256]; WCHAR fenceName[256];
};

// --- D3D12 Globals ---
static const UINT kFrameCount = 2;
static ComPtr<ID3D12Device>           g_device;
static ComPtr<ID3D12CommandQueue>     g_commandQueue;
static ComPtr<IDXGISwapChain3>        g_swapChain;
static ComPtr<ID3D12Resource>         g_renderTargets[kFrameCount];
static ComPtr<ID3D12CommandAllocator> g_commandAllocators[kFrameCount];
static ComPtr<ID3D12GraphicsCommandList> g_commandList;
static ComPtr<ID3D12DescriptorHeap>   g_rtvHeap;
static ComPtr<ID3D12RootSignature>    g_rootSignature;
static ComPtr<ID3D12PipelineState>    g_pso;
static ComPtr<ID3D12PipelineState>    g_passthroughPSO; // PSO for preview window
static UINT                           g_rtvDescriptorSize;
static UINT                           g_srvDescriptorSize;
static UINT                           g_frameIndex;
static ComPtr<ID3D12Fence>            g_renderFence;
static UINT64                         g_renderFenceValues[kFrameCount] = {};
static HANDLE                         g_fenceEvent;
static UINT64                         g_fenceValue = 1;
static HWND                           g_hwnd = nullptr;

// --- Multiplexer Output (Producer) Globals ---
static const UINT MUX_WIDTH = 1920;
static const UINT MUX_HEIGHT = 1080;
static ComPtr<ID3D12Resource>         g_compositeTexture;
static ComPtr<ID3D12DescriptorHeap>   g_compositeRtvHeap;
static ComPtr<ID3D12DescriptorHeap>   g_compositeSrvHeap; // SRV heap for the final texture
static ComPtr<ID3D12Resource>         g_sharedOutTexture;
static ComPtr<ID3D12Fence>            g_sharedOutFence;
static UINT64                         g_sharedOutFrameValue = 0;
static HANDLE                         g_hManifestOut = nullptr;
static BroadcastManifest*             g_pManifestViewOut = nullptr;
static HANDLE                         g_sharedOutTextureHandle = nullptr;
static HANDLE                         g_sharedOutFenceHandle = nullptr;

// --- Input (Consumer) Globals ---
const int MAX_PRODUCERS = 256; // <<< INCREASED LIMIT
struct ProducerConnection {
    bool isConnected = false; DWORD producerPid = 0;
    HANDLE hManifest = nullptr; BroadcastManifest* pManifestView = nullptr;
    ComPtr<ID3D12Resource> sharedTexture; ComPtr<ID3D12Fence> sharedFence;
    UINT64 lastSeenFrame = 0; ComPtr<ID3D12Resource> privateTexture;
    UINT srvDescriptorIndex = 0;
};
static ProducerConnection g_producers[MAX_PRODUCERS];
static ComPtr<ID3D12DescriptorHeap>   g_srvHeap;

// --- Shader ---
const char* g_shaderHLSL = R"(
Texture2D g_texture : register(t0);
SamplerState g_sampler : register(s0);
struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
PSInput VSMain(uint id : SV_VertexID) {
    PSInput o; float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1);
    o.uv = uv; return o;
}
float4 PSMain(PSInput i) : SV_TARGET { return g_texture.Sample(g_sampler, i.uv); }
)";

// --- Forward Declarations ---
static void InitD3D12(HWND hwnd);
static void LoadAssets();
static void PopulateCommandList();
static void FindAndConnectToProducers();
static void DisconnectFromProducer(int i);
static void UpdateWindowTitle();
static HRESULT InitializeSharing();
static void Cleanup();
static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static void MoveToNextFrame();
static void WaitForGpuIdle();

// --- Main Loop ---
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    const WCHAR szClassName[] = L"DirectPortMultiplexerD3D12Wnd";
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEXW) };
    wcex.lpfnWndProc = WndProc; wcex.hInstance = hInstance; wcex.lpszClassName = szClassName;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(DPD_ICON1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    wcex.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(DPD_ICON1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    RegisterClassExW(&wcex);

    RECT rc = { 0, 0, 1280, 720 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(0, szClassName, L"", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInstance, nullptr);
    UpdateWindowTitle();

    SendMessage(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)wcex.hIcon);
    SendMessage(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)wcex.hIconSm);

    InitD3D12(g_hwnd);
    LoadAssets();
    InitializeSharing();
    ShowWindow(g_hwnd, nCmdShow);

    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessage(&msg);
        } else {
            FindAndConnectToProducers();
            PopulateCommandList();
            ID3D12CommandList* ppCommandLists[] = { g_commandList.Get() };
            g_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
            
            g_commandQueue->Signal(g_sharedOutFence.Get(), ++g_sharedOutFrameValue);
            if (g_pManifestViewOut) {
                g_pManifestViewOut->frameValue = g_sharedOutFrameValue;
                WakeByAddressAll(&g_pManifestViewOut->frameValue);
            }

            g_swapChain->Present(1, 0);
            MoveToNextFrame();
        }
    }
    Cleanup();
    return static_cast<int>(msg.wParam);
}


// --- Core Rendering and Logic ---

void PopulateCommandList() {
    g_commandAllocators[g_frameIndex]->Reset();
    g_commandList->Reset(g_commandAllocators[g_frameIndex].Get(), g_pso.Get());

    // --- STAGE 1: CONSUME (Sync and Copy) ---
    std::vector<D3D12_RESOURCE_BARRIER> preCopyBarriers;
    for (int i = 0; i < MAX_PRODUCERS; ++i) {
        auto& producer = g_producers[i];
        if (producer.isConnected && producer.privateTexture) {
            UINT64 latestFrame = producer.pManifestView->frameValue;
            if (latestFrame > producer.lastSeenFrame) {
                g_commandQueue->Wait(producer.sharedFence.Get(), latestFrame);
                
                D3D12_RESOURCE_BARRIER barrier = {};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                barrier.Transition.pResource = producer.privateTexture.Get();
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                preCopyBarriers.push_back(barrier);
            }
        }
    }
    if (!preCopyBarriers.empty()) g_commandList->ResourceBarrier((UINT)preCopyBarriers.size(), preCopyBarriers.data());

    for (int i = 0; i < MAX_PRODUCERS; ++i) {
        auto& producer = g_producers[i];
        if (producer.isConnected) {
            UINT64 latestFrame = producer.pManifestView->frameValue;
            if (latestFrame > producer.lastSeenFrame) {
                g_commandList->CopyResource(producer.privateTexture.Get(), producer.sharedTexture.Get());
                producer.lastSeenFrame = latestFrame;
            }
        }
    }

    std::vector<D3D12_RESOURCE_BARRIER> postCopyBarriers;
    for (const auto& barrier : preCopyBarriers) {
        D3D12_RESOURCE_BARRIER postBarrier = barrier;
        postBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        postBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        postCopyBarriers.push_back(postBarrier);
    }
    if (!postCopyBarriers.empty()) g_commandList->ResourceBarrier((UINT)postCopyBarriers.size(), postCopyBarriers.data());

    // --- STAGE 2: COMPOSE ---
    D3D12_RESOURCE_BARRIER compositeToRTVBarrier = {};
    compositeToRTVBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    compositeToRTVBarrier.Transition.pResource = g_compositeTexture.Get();
    compositeToRTVBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    compositeToRTVBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    compositeToRTVBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_commandList->ResourceBarrier(1, &compositeToRTVBarrier);
    
    D3D12_CPU_DESCRIPTOR_HANDLE compositeRtvHandle = g_compositeRtvHeap->GetCPUDescriptorHandleForHeapStart();
    g_commandList->OMSetRenderTargets(1, &compositeRtvHandle, FALSE, nullptr);
    const float clearColor[] = { 0.05f, 0.0f, 0.05f, 1.0f }; // Dark purple background
    g_commandList->ClearRenderTargetView(compositeRtvHandle, clearColor, 0, nullptr);
    
    g_commandList->SetGraphicsRootSignature(g_rootSignature.Get());
    ID3D12DescriptorHeap* ppHeaps[] = { g_srvHeap.Get() };
    g_commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
    g_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    std::vector<int> activeProducers;
    for(int i=0; i < MAX_PRODUCERS; ++i) if(g_producers[i].isConnected) activeProducers.push_back(i);

    if (!activeProducers.empty()) {
        int count = (int)activeProducers.size();
        int cols = static_cast<int>(ceil(sqrt(static_cast<float>(count))));
        int rows = (count + cols - 1) / cols;

        for (int i = 0; i < count; ++i) {
            int producerIndex = activeProducers[i];
            int gridCol = i % cols;
            // --- FIX: Correct grid row calculation ---
            int gridRow = i / cols;

            int left   = (gridCol * MUX_WIDTH) / cols;
            int right  = ((gridCol + 1) * MUX_WIDTH) / cols;
            int top    = (gridRow * MUX_HEIGHT) / rows;
            int bottom = ((gridRow + 1) * MUX_HEIGHT) / rows;

            D3D12_VIEWPORT vp = {};
            vp.TopLeftX = static_cast<float>(left);
            vp.TopLeftY = static_cast<float>(top);
            vp.Width    = static_cast<float>(right - left);
            vp.Height   = static_cast<float>(bottom - top);
            vp.MinDepth = 0.0f;
            vp.MaxDepth = 1.0f;
            
            D3D12_RECT sr = {};
            sr.left   = left;
            sr.top    = top;
            sr.right  = right;
            sr.bottom = bottom;

            g_commandList->RSSetViewports(1, &vp);
            g_commandList->RSSetScissorRects(1, &sr);

            D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
            srvHandle.ptr += (UINT64)g_producers[producerIndex].srvDescriptorIndex * g_srvDescriptorSize;
            g_commandList->SetGraphicsRootDescriptorTable(0, srvHandle);
            g_commandList->DrawInstanced(3, 1, 0, 0);
        }
    }
    
    // --- STAGE 3: PRODUCE ---
    D3D12_RESOURCE_BARRIER barriersToProduce[2] = {};
    barriersToProduce[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriersToProduce[0].Transition = { g_compositeTexture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE };
    barriersToProduce[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriersToProduce[1].Transition = { g_sharedOutTexture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST };
    g_commandList->ResourceBarrier(2, barriersToProduce);

    g_commandList->CopyResource(g_sharedOutTexture.Get(), g_compositeTexture.Get());
    
    D3D12_RESOURCE_BARRIER barriersAfterProduce[2] = {};
    barriersAfterProduce[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriersAfterProduce[0].Transition = { g_compositeTexture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
    barriersAfterProduce[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriersAfterProduce[1].Transition = { g_sharedOutTexture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON };
    g_commandList->ResourceBarrier(2, barriersAfterProduce);
    
    // --- STAGE 4: PRESENT (Local Preview) using a blit ---
    D3D12_RESOURCE_BARRIER presentBarrier = {};
    presentBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    presentBarrier.Transition = { g_renderTargets[g_frameIndex].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET };
    g_commandList->ResourceBarrier(1, &presentBarrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += (g_frameIndex * g_rtvDescriptorSize);
    g_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    const float previewClearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    g_commandList->ClearRenderTargetView(rtvHandle, previewClearColor, 0, nullptr);
    
    // Set up for the passthrough draw
    g_commandList->SetPipelineState(g_passthroughPSO.Get());
    ID3D12DescriptorHeap* previewHeaps[] = { g_compositeSrvHeap.Get() };
    g_commandList->SetDescriptorHeaps(_countof(previewHeaps), previewHeaps);
    g_commandList->SetGraphicsRootDescriptorTable(0, g_compositeSrvHeap->GetGPUDescriptorHandleForHeapStart());

    // Set viewport to the window's current size
    RECT clientRect;
    GetClientRect(g_hwnd, &clientRect);
    D3D12_VIEWPORT presentVp = {};
    presentVp.Width    = static_cast<float>(clientRect.right - clientRect.left);
    presentVp.Height   = static_cast<float>(clientRect.bottom - clientRect.top);
    presentVp.MinDepth = 0.0f;
    presentVp.MaxDepth = 1.0f;
    D3D12_RECT presentSr = { 0, 0, (LONG)presentVp.Width, (LONG)presentVp.Height };
    g_commandList->RSSetViewports(1, &presentVp);
    g_commandList->RSSetScissorRects(1, &presentSr);
    
    g_commandList->DrawInstanced(3, 1, 0, 0);

    // Transition back to present state
    presentBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    presentBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_commandList->ResourceBarrier(1, &presentBarrier);

    g_commandList->Close();
}

// --- Setup, Teardown, and Discovery ---
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void FindAndConnectToProducers() {
    static auto lastSearchTime = std::chrono::steady_clock::now() - std::chrono::seconds(2);

    for (int i = 0; i < MAX_PRODUCERS; ++i) {
        if (!g_producers[i].isConnected) continue;
        HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, g_producers[i].producerPid);
        if (hProcess == NULL || WaitForSingleObject(hProcess, 0) != WAIT_TIMEOUT) {
            DisconnectFromProducer(i);
        }
        if (hProcess) CloseHandle(hProcess);
    }

    if (std::chrono::steady_clock::now() - lastSearchTime < std::chrono::seconds(1)) return;
    lastSearchTime = std::chrono::steady_clock::now();
    
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe32 = {}; pe32.dwSize = sizeof(PROCESSENTRY32W);
    DWORD selfPid = GetCurrentProcessId();

    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            if (pe32.th32ProcessID == selfPid) continue;
            
            bool alreadyConnected = false;
            for (int i = 0; i < MAX_PRODUCERS; ++i) {
                if (g_producers[i].isConnected && g_producers[i].producerPid == pe32.th32ProcessID) {
                    alreadyConnected = true; break;
                }
            }
            if (alreadyConnected) continue;

            int availableSlot = -1;
            for (int i = 0; i < MAX_PRODUCERS; ++i) if (!g_producers[i].isConnected) { availableSlot = i; break; }
            if (availableSlot == -1) { CloseHandle(hSnapshot); return; }

            const std::vector<std::wstring> prefixes = { L"D3D12_Producer_Manifest_", L"DirectPort_Producer_Manifest_" };
            HANDLE hManifest = nullptr;
            for (const auto& prefix : prefixes) {
                std::wstring manifestName = prefix + std::to_wstring(pe32.th32ProcessID);
                hManifest = OpenFileMappingW(FILE_MAP_READ, FALSE, manifestName.c_str());
                if (hManifest) break;
            }
            if (!hManifest) continue;
            
            BroadcastManifest* pManifestView = (BroadcastManifest*)MapViewOfFile(hManifest, FILE_MAP_READ, 0, 0, sizeof(BroadcastManifest));
            if (!pManifestView) { CloseHandle(hManifest); continue; }

            auto& producer = g_producers[availableSlot];
            HANDLE hTexture, hFence;
            g_device->OpenSharedHandleByName(pManifestView->textureName, GENERIC_ALL, &hTexture);
            g_device->OpenSharedHandleByName(pManifestView->fenceName, GENERIC_ALL, &hFence);

            if (hTexture && hFence) {
                g_device->OpenSharedHandle(hTexture, IID_PPV_ARGS(&producer.sharedTexture));
                g_device->OpenSharedHandle(hFence, IID_PPV_ARGS(&producer.sharedFence));
            }
            CloseHandle(hTexture); CloseHandle(hFence);

            if (producer.sharedTexture && producer.sharedFence) {
                producer.isConnected = true;
                producer.producerPid = pe32.th32ProcessID;
                producer.hManifest = hManifest;
                producer.pManifestView = pManifestView;
                producer.lastSeenFrame = (pManifestView->frameValue > 0) ? (pManifestView->frameValue - 1) : 0;
                producer.srvDescriptorIndex = availableSlot;
                
                D3D12_RESOURCE_DESC desc = producer.sharedTexture->GetDesc();
                desc.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET; // Private copy is not a render target
                
                D3D12_HEAP_PROPERTIES defaultHeapProps = {};
                defaultHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

                g_device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&producer.privateTexture));

                D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
                srvHandle.ptr += (UINT64)producer.srvDescriptorIndex * g_srvDescriptorSize;
                
                g_device->CreateShaderResourceView(producer.privateTexture.Get(), nullptr, srvHandle);

                Log(L"Connected to producer PID: " + std::to_wstring(pe32.th32ProcessID) + L" in slot " + std::to_wstring(availableSlot));
                UpdateWindowTitle();
            } else {
                UnmapViewOfFile(pManifestView); CloseHandle(hManifest);
            }
        } while (Process32NextW(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
}

void DisconnectFromProducer(int i) {
    auto& p = g_producers[i];
    if (!p.isConnected) return;
    Log(L"Disconnecting from producer PID: " + std::to_wstring(p.producerPid));
    WaitForGpuIdle();
    if (p.pManifestView) UnmapViewOfFile(p.pManifestView);
    if (p.hManifest) CloseHandle(p.hManifest);
    p = {}; // Reset struct
    UpdateWindowTitle();
}

void InitD3D12(HWND hwnd) {
    ComPtr<IDXGIFactory4> factory; CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device));
    D3D12_COMMAND_QUEUE_DESC queueDesc = {}; queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_commandQueue));
    RECT rc; GetClientRect(hwnd, &rc);
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = kFrameCount; swapChainDesc.Width = rc.right - rc.left; swapChainDesc.Height = rc.bottom - rc.top;
    swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; swapChainDesc.SampleDesc.Count = 1;
    ComPtr<IDXGISwapChain1> swapChain;
    factory->CreateSwapChainForHwnd(g_commandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &swapChain);
    swapChain.As(&g_swapChain); g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {}; rtvHeapDesc.NumDescriptors = kFrameCount; rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    g_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_rtvHeap));
    g_rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT n = 0; n < kFrameCount; n++) {
        g_swapChain->GetBuffer(n, IID_PPV_ARGS(&g_renderTargets[n]));
        g_device->CreateRenderTargetView(g_renderTargets[n].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += g_rtvDescriptorSize;
    }
    for (UINT n = 0; n < kFrameCount; n++) g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_commandAllocators[n]));
    g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_commandAllocators[g_frameIndex].Get(), nullptr, IID_PPV_ARGS(&g_commandList));
    g_commandList->Close();
    g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_renderFence));
    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

void LoadAssets() {
    D3D12_DESCRIPTOR_RANGE ranges[1] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0; ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER rootParameters[1] = {};
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[0].DescriptorTable = { 1, ranges };
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    
    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
    rootSignatureDesc.NumParameters = _countof(rootParameters);
    rootSignatureDesc.pParameters = rootParameters;
    rootSignatureDesc.NumStaticSamplers = 1;
    rootSignatureDesc.pStaticSamplers = &sampler;
    rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature, error;
    D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    g_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&g_rootSignature));
    ComPtr<ID3DBlob> vs, ps;
    D3DCompile(g_shaderHLSL, strlen(g_shaderHLSL), nullptr, nullptr, nullptr, "VSMain", "vs_5_1", 0, 0, &vs, nullptr);
    D3DCompile(g_shaderHLSL, strlen(g_shaderHLSL), nullptr, nullptr, nullptr, "PSMain", "ps_5_1", 0, 0, &ps, nullptr);
    
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = g_rootSignature.Get();
    psoDesc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    psoDesc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };

    D3D12_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
    rasterizerDesc.FrontCounterClockwise = FALSE;
    rasterizerDesc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rasterizerDesc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rasterizerDesc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    rasterizerDesc.DepthClipEnable = TRUE;
    rasterizerDesc.MultisampleEnable = FALSE;
    rasterizerDesc.AntialiasedLineEnable = FALSE;
    rasterizerDesc.ForcedSampleCount = 0;
    rasterizerDesc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    psoDesc.RasterizerState = rasterizerDesc;

    D3D12_BLEND_DESC blendDesc = {};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    const D3D12_RENDER_TARGET_BLEND_DESC defaultRenderTargetBlendDesc = { FALSE,FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL, };
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) blendDesc.RenderTarget[i] = defaultRenderTargetBlendDesc;
    psoDesc.BlendState = blendDesc;

    psoDesc.SampleMask = UINT_MAX; psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1; psoDesc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM; psoDesc.SampleDesc.Count = 1;
    g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_pso));
    g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_passthroughPSO));

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = MAX_PRODUCERS;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&g_srvHeap));
    g_srvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

HRESULT InitializeSharing() {
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {}; rtvHeapDesc.NumDescriptors = 1; rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    g_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_compositeRtvHeap));

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {}; srvHeapDesc.NumDescriptors = 1; srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&g_compositeSrvHeap));
    
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.Width = MUX_WIDTH;
    texDesc.Height = MUX_HEIGHT;
    texDesc.MipLevels = 1;
    texDesc.DepthOrArraySize = 1;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES defaultHeapProps = {};
    defaultHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    g_device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&g_compositeTexture));
    g_device->CreateRenderTargetView(g_compositeTexture.Get(), nullptr, g_compositeRtvHeap->GetCPUDescriptorHandleForHeapStart());
    g_device->CreateShaderResourceView(g_compositeTexture.Get(), nullptr, g_compositeSrvHeap->GetCPUDescriptorHandleForHeapStart());

    texDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    g_device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_SHARED, &texDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_sharedOutTexture));
    
    PSECURITY_DESCRIPTOR sd = nullptr; SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, FALSE };
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd, NULL);
    sa.lpSecurityDescriptor = sd;
    
    DWORD pid = GetCurrentProcessId();
    std::wstring textureName = L"Global\\DirectPortTexture_Multiplexer_" + std::to_wstring(pid);
    std::wstring fenceName = L"Global\\DirectPortFence_Multiplexer_" + std::to_wstring(pid);
    g_device->CreateSharedHandle(g_sharedOutTexture.Get(), &sa, GENERIC_ALL, textureName.c_str(), &g_sharedOutTextureHandle);
    g_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_sharedOutFence));
    g_device->CreateSharedHandle(g_sharedOutFence.Get(), &sa, GENERIC_ALL, fenceName.c_str(), &g_sharedOutFenceHandle);
    
    std::wstring manifestName = L"DirectPort_Producer_Manifest_" + std::to_wstring(pid);
    g_hManifestOut = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(BroadcastManifest), manifestName.c_str());
    LocalFree(sd);
    if (!g_hManifestOut) return E_FAIL;
    g_pManifestViewOut = (BroadcastManifest*)MapViewOfFile(g_hManifestOut, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    ZeroMemory(g_pManifestViewOut, sizeof(BroadcastManifest));
    g_pManifestViewOut->width = MUX_WIDTH; g_pManifestViewOut->height = MUX_HEIGHT; g_pManifestViewOut->format = DXGI_FORMAT_B8G8R8A8_UNORM;
    g_pManifestViewOut->adapterLuid = g_device->GetAdapterLuid();
    wcscpy_s(g_pManifestViewOut->textureName, textureName.c_str());
    wcscpy_s(g_pManifestViewOut->fenceName, fenceName.c_str());
    return S_OK;
}

void UpdateWindowTitle() {
    int count = 0; for(int i=0; i<MAX_PRODUCERS; ++i) if(g_producers[i].isConnected) count++;
    std::wstring title = L"Multiplexer (D3D12) | Consuming " + std::to_wstring(count) + L" | Producing (PID: " + std::to_wstring(GetCurrentProcessId()) + L")";
    SetWindowTextW(g_hwnd, title.c_str());
}

void Cleanup() {
    WaitForGpuIdle();
    for(int i=0; i < MAX_PRODUCERS; ++i) DisconnectFromProducer(i);
    if (g_pManifestViewOut) UnmapViewOfFile(g_pManifestViewOut);
    if (g_hManifestOut) CloseHandle(g_hManifestOut);
    if (g_sharedOutFenceHandle) CloseHandle(g_sharedOutFenceHandle);
    if (g_sharedOutTextureHandle) CloseHandle(g_sharedOutTextureHandle);
    CloseHandle(g_fenceEvent);
}

void MoveToNextFrame() {
    g_commandQueue->Signal(g_renderFence.Get(), g_fenceValue);
    g_renderFenceValues[g_frameIndex] = g_fenceValue;
    g_fenceValue++;
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
    if (g_renderFence->GetCompletedValue() < g_renderFenceValues[g_frameIndex]) {
        g_renderFence->SetEventOnCompletion(g_renderFenceValues[g_frameIndex], g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }
}

void WaitForGpuIdle() {
    g_commandQueue->Signal(g_renderFence.Get(), g_fenceValue);
    g_renderFence->SetEventOnCompletion(g_fenceValue, g_fenceEvent);
    WaitForSingleObject(g_fenceEvent, INFINITE);
    g_fenceValue++;
}