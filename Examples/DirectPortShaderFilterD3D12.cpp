// --- DirectPortShaderFilterD3D12.cpp ---
// A dynamic, auto-discovering filter.
// 1. Automatically finds and connects to the first available producer.
// 2. Initially acts in "Passthrough" mode, displaying and re-sharing the original texture.
// 3. On pressing SPACE, allows loading a shader to switch to "Filter" mode.
// 4. In Filter mode, it applies the shader and shares the processed result.

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
#include <commdlg.h>
#include <tlhelp32.h>
#include "resource.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "Synchronization.lib")
#pragma comment(lib, "Comdlg32.lib")

using namespace Microsoft::WRL;

// --- Logging ---
void Log(const std::wstring& msg) { WCHAR b[512]; wsprintfW(b, L"[PID:%lu][ShaderFilter] %s\n", GetCurrentProcessId(), msg.c_str()); OutputDebugStringW(b); }
void LogHRESULT(const std::wstring& msg, HRESULT hr) { WCHAR b[512]; wsprintfW(b, L"[PID:%lu][ShaderFilter] %s - HRESULT: 0x%08X\n", GetCurrentProcessId(), msg.c_str(), hr); OutputDebugStringW(b); }

// --- Core Structs ---
struct BroadcastManifest {
    UINT64 frameValue; UINT width; UINT height; DXGI_FORMAT format;
    LUID adapterLuid; WCHAR textureName[256]; WCHAR fenceName[256];
};
struct ConstantBuffer { float bar_rect[4]; float resolution[2]; float time; float pad; };

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
static ComPtr<ID3D12PipelineState>    g_passthroughPSO;
static ComPtr<ID3D12PipelineState>    g_dynamicPSO;
static UINT                           g_rtvDescriptorSize;
static UINT                           g_frameIndex;
static ComPtr<ID3D12Fence>            g_renderFence;
static UINT64                         g_renderFenceValues[kFrameCount] = {};
static HANDLE                         g_fenceEvent;
static UINT64                         g_fenceValue = 1;
static HWND                           g_hwnd = nullptr;

static ComPtr<ID3D12Resource>         g_constantBuffer;
static void*                          g_pCbvDataBegin = nullptr;
static auto                           gStartTime = std::chrono::high_resolution_clock::now();
static float                          gTime = 0.0f;

// --- Output (Producer) Globals ---
static ComPtr<ID3D12Resource>         g_sharedTexture;
static ComPtr<ID3D12DescriptorHeap>   g_sharedRtvHeap;
static ComPtr<ID3D12DescriptorHeap>   g_sharedSrvHeap; // SRV for the output texture for blitting
static ComPtr<ID3D12Fence>            g_sharedFence;
static UINT64                         g_sharedFrameValue = 0;
static HANDLE                         g_hManifest = nullptr;
static BroadcastManifest*             g_pManifestView = nullptr;
static std::wstring                   g_sharedTextureName, g_sharedFenceName;
static HANDLE                         g_sharedTextureHandle = nullptr;
static HANDLE                         g_sharedFenceHandle = nullptr;

// --- Input (Consumer) Globals ---
static bool                           g_isConnected = false;
static DWORD                          g_connectedProducerPid = 0;
static ComPtr<ID3D12Resource>         g_inputTexture;
static ComPtr<ID3D12Fence>            g_inputFence;
static BroadcastManifest              g_inputManifest = {};
static HANDLE                         g_hInputManifest = nullptr;
static BroadcastManifest*             g_pInputManifestView = nullptr;
static UINT64                         g_lastWaitedInputFrameValue = 0;
static ComPtr<ID3D12DescriptorHeap>   g_srvHeap;

const char* g_VertexShaderHLSL = R"(
struct VOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD;
};
VOut main(uint vid : SV_VertexID) {
    float2 uv = float2((vid << 1) & 2, vid & 2);
    VOut o;
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1);
    o.uv = uv;
    return o;
})";

const char* g_PassthroughShaderHLSL = R"(
Texture2D    inputTexture  : register(t0);
SamplerState linearSampler : register(s0);
struct VIn {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD;
};
float4 main(VIn i) : SV_Target {
    return inputTexture.Sample(linearSampler, i.uv);
})";

// --- Forward Declarations ---
static void InitD3D12(HWND hwnd);
static void LoadAssets();
static HRESULT InitializeSharing(UINT width, UINT height);
static void DisconnectFromProducer();
static void FindAndConnectToProducer();
static void UpdateWindowTitle();
static void MoveToNextFrame();
static void WaitForGpuIdle();
static void Cleanup();
static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static void OnResize(UINT width, UINT height);
static bool LoadShaderAndCreatePSO(HWND hwnd, bool isPassthrough = false);
static void UpdateTime();
static void PopulateCommandList();

// --- Main and Window ---
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    const WCHAR szClassName[] = L"DirectPortShaderFilterD3D12Wnd";
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEXW) };
    wcex.lpfnWndProc = WndProc; wcex.hInstance = hInstance; wcex.lpszClassName = szClassName;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(DPD_ICON1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    wcex.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(DPD_ICON1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    RegisterClassExW(&wcex);
    
    DWORD pid = GetCurrentProcessId();
    g_sharedTextureName = L"Global\\DirectPortTexture_" + std::to_wstring(pid);
    g_sharedFenceName = L"Global\\DirectPortFence_" + std::to_wstring(pid);
    
    RECT rc = { 0, 0, 1280, 720 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(0, szClassName, L"", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInstance, nullptr);
    UpdateWindowTitle(); // Sets initial "Searching..." title

    SendMessage(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)wcex.hIcon);
    SendMessage(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)wcex.hIconSm);

    InitD3D12(g_hwnd);
    LoadAssets();

    ShowWindow(g_hwnd, nCmdShow);

    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            FindAndConnectToProducer();
            UpdateTime();
            PopulateCommandList();
            ID3D12CommandList* ppCommandLists[] = { g_commandList.Get() };
            g_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
            
            if (g_isConnected) {
                g_commandQueue->Signal(g_sharedFence.Get(), ++g_sharedFrameValue);
                if (g_pManifestView) {
                    g_pManifestView->frameValue = g_sharedFrameValue;
                    WakeByAddressAll(&g_pManifestView->frameValue);
                }
            }
            g_swapChain->Present(1, 0);
            MoveToNextFrame();
        }
    }
    Cleanup();
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_KEYDOWN:
            if (wParam == VK_SPACE && g_isConnected) {
                WaitForGpuIdle();
                LoadShaderAndCreatePSO(hwnd, false);
                return 0;
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_SIZE:
            if (g_device && wParam != SIZE_MINIMIZED) {
                OnResize(LOWORD(lParam), HIWORD(lParam));
            }
            return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// --- Core Logic ---
void PopulateCommandList() {
    g_commandAllocators[g_frameIndex]->Reset();

    ID3D12PipelineState* pso = g_isConnected ? (g_dynamicPSO ? g_dynamicPSO.Get() : g_passthroughPSO.Get()) : nullptr;
    
    g_commandList->Reset(g_commandAllocators[g_frameIndex].Get(), pso);
    
    if (g_isConnected && pso) {
        UINT64 latestInputFrame = g_pInputManifestView->frameValue;
        if (latestInputFrame > g_lastWaitedInputFrameValue) {
            g_commandQueue->Wait(g_inputFence.Get(), latestInputFrame);
            g_lastWaitedInputFrameValue = latestInputFrame;
        }

        g_commandList->SetGraphicsRootSignature(g_rootSignature.Get());
        
        ID3D12DescriptorHeap* ppHeaps[] = { g_srvHeap.Get() };
        g_commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
        g_commandList->SetGraphicsRootConstantBufferView(0, g_constantBuffer->GetGPUVirtualAddress());
        g_commandList->SetGraphicsRootDescriptorTable(1, g_srvHeap->GetGPUDescriptorHandleForHeapStart());

        D3D12_RESOURCE_DESC sharedDesc = g_sharedTexture->GetDesc();
        D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)sharedDesc.Width, (float)sharedDesc.Height, 0.0f, 1.0f };
        D3D12_RECT scissorRect = { 0, 0, (LONG)sharedDesc.Width, (LONG)sharedDesc.Height };
        g_commandList->RSSetViewports(1, &viewport);
        g_commandList->RSSetScissorRects(1, &scissorRect);
        g_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition = { g_inputTexture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Transition = { g_sharedTexture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET };
        g_commandList->ResourceBarrier(2, barriers);

        D3D12_CPU_DESCRIPTOR_HANDLE sharedRtvHandle = g_sharedRtvHeap->GetCPUDescriptorHandleForHeapStart();
        g_commandList->OMSetRenderTargets(1, &sharedRtvHandle, FALSE, nullptr);
        
        const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f }; 
        g_commandList->ClearRenderTargetView(sharedRtvHandle, clearColor, 0, nullptr);

        g_commandList->DrawInstanced(3, 1, 0, 0);
        
        D3D12_RESOURCE_BARRIER postRenderBarriers[2] = {};
        postRenderBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        postRenderBarriers[0].Transition = { g_inputTexture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON };
        postRenderBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        postRenderBarriers[1].Transition = { g_sharedTexture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        g_commandList->ResourceBarrier(2, postRenderBarriers);

        // --- BLIT TO SWAPCHAIN ---
        D3D12_RESOURCE_BARRIER presentBarrier = {};
        presentBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        presentBarrier.Transition = { g_renderTargets[g_frameIndex].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET };
        g_commandList->ResourceBarrier(1, &presentBarrier);
        
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtvHandle.ptr += g_frameIndex * g_rtvDescriptorSize;
        g_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
        const float previewClear[] = { 0.0f, 0.0f, 0.0f, 1.0f };
        g_commandList->ClearRenderTargetView(rtvHandle, previewClear, 0, nullptr);

        g_commandList->SetPipelineState(g_passthroughPSO.Get());
        ID3D12DescriptorHeap* ppPreviewHeaps[] = { g_sharedSrvHeap.Get() };
        g_commandList->SetDescriptorHeaps(_countof(ppPreviewHeaps), ppPreviewHeaps);
        g_commandList->SetGraphicsRootDescriptorTable(1, g_sharedSrvHeap->GetGPUDescriptorHandleForHeapStart());

        RECT clientRect; GetClientRect(g_hwnd, &clientRect);
        D3D12_VIEWPORT previewVp = { 0.0f, 0.0f, (float)(clientRect.right - clientRect.left), (float)(clientRect.bottom - clientRect.top), 0.0f, 1.0f };
        D3D12_RECT previewSr = { 0, 0, (LONG)previewVp.Width, (LONG)previewVp.Height };
        g_commandList->RSSetViewports(1, &previewVp);
        g_commandList->RSSetScissorRects(1, &previewSr);
        g_commandList->DrawInstanced(3, 1, 0, 0);

        presentBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        presentBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_commandList->ResourceBarrier(1, &presentBarrier);
    } else {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = { g_renderTargets[g_frameIndex].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET };
        g_commandList->ResourceBarrier(1, &barrier);

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtvHandle.ptr += g_frameIndex * g_rtvDescriptorSize;
        g_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
        const float clearColor[] = { 0.1f, 0.1f, 0.1f, 1.0f };
        g_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_commandList->ResourceBarrier(1, &barrier);
    }
    g_commandList->Close();
}

void UpdateTime() {
    if (!g_isConnected) return;
    gTime = std::chrono::duration<float>(std::chrono::high_resolution_clock::now() - gStartTime).count();
    D3D12_RESOURCE_DESC desc = g_sharedTexture->GetDesc();
    ConstantBuffer cb{};
    cb.resolution[0] = (float)desc.Width;
    cb.resolution[1] = (float)desc.Height;
    cb.time = gTime;
    if (g_pCbvDataBegin) {
        memcpy(g_pCbvDataBegin, &cb, sizeof(cb));
    }
}

// --- Setup, Teardown, and Utility Functions ---

void InitD3D12(HWND hwnd) {
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
    }
#endif
    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device));
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_commandQueue));
    RECT rc; GetClientRect(hwnd, &rc);
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = kFrameCount;
    swapChainDesc.Width = rc.right - rc.left;
    swapChainDesc.Height = rc.bottom - rc.top;
    swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;
    ComPtr<IDXGISwapChain1> swapChain;
    factory->CreateSwapChainForHwnd(g_commandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &swapChain);
    swapChain.As(&g_swapChain);
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = kFrameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    g_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_rtvHeap));
    g_rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT n = 0; n < kFrameCount; n++) {
        g_swapChain->GetBuffer(n, IID_PPV_ARGS(&g_renderTargets[n]));
        g_device->CreateRenderTargetView(g_renderTargets[n].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += g_rtvDescriptorSize;
    }
    for (UINT n = 0; n < kFrameCount; n++) {
        g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_commandAllocators[n]));
    }
    g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_commandAllocators[g_frameIndex].Get(), nullptr, IID_PPV_ARGS(&g_commandList));
    g_commandList->Close();
    g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_renderFence));
    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

void LoadAssets() {
    D3D12_DESCRIPTOR_RANGE ranges[1] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParameters[2] = {};
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[0].Descriptor = { 0, 0 };
    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[1].DescriptorTable = { 1, ranges };

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.ShaderRegister = 0;
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
    
    D3D12_HEAP_PROPERTIES uploadHeapProps = {D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = (sizeof(ConstantBuffer) + 255) & ~255; 
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    g_device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_constantBuffer));
    D3D12_RANGE readRange = {};
    g_constantBuffer->Map(0, &readRange, &g_pCbvDataBegin);
}

bool LoadShaderAndCreatePSO(HWND hwnd, bool isPassthrough) {
    ComPtr<ID3DBlob> psBlob;
    std::wstring loadedShaderPath;
    if (isPassthrough) {
        D3DCompile(g_PassthroughShaderHLSL, strlen(g_PassthroughShaderHLSL), nullptr, nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &psBlob, nullptr);
    } else {
        wchar_t path[MAX_PATH] = L"";
        OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd; ofn.lpstrFilter = L"Shader Files (*.cso, *.hlsl)\0*.cso;*.hlsl\0All Files\0*.*\0";
        ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
        if (!GetOpenFileNameW(&ofn)) return false;
        loadedShaderPath = path;

        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) { return false; }
        DWORD size = GetFileSize(f, nullptr);
        std::vector<char> buffer(size);
        DWORD read = 0;
        ReadFile(f, buffer.data(), size, &read, nullptr);
        CloseHandle(f);
        
        std::wstring ext = path;
        ext = ext.substr(ext.find_last_of(L".") + 1);
        if (_wcsicmp(ext.c_str(), L"hlsl") == 0) {
            ComPtr<ID3DBlob> errorBlob;
            if (FAILED(D3DCompile(buffer.data(), buffer.size(), nullptr, nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &psBlob, &errorBlob))) {
                if (errorBlob) MessageBoxA(hwnd, (char*)errorBlob->GetBufferPointer(), "HLSL Compile Error", MB_OK);
                return false;
            }
        } else {
            if (FAILED(D3DCreateBlob(buffer.size(), &psBlob))) return false;
            memcpy(psBlob->GetBufferPointer(), buffer.data(), buffer.size());
        }
    }
    
    ComPtr<ID3DBlob> vsBlob;
    const char* vsSource = g_VertexShaderHLSL;
    D3DCompile(vsSource, strlen(vsSource), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, nullptr);
    
    D3D12_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID; rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
    rasterizerDesc.DepthClipEnable = TRUE;
    D3D12_BLEND_DESC blendDesc = {};
    const D3D12_RENDER_TARGET_BLEND_DESC defaultRenderTargetBlendDesc = { FALSE,FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL, };
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) blendDesc.RenderTarget[i] = defaultRenderTargetBlendDesc;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = g_rootSignature.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.RasterizerState = rasterizerDesc;
    psoDesc.BlendState = blendDesc;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> newPSO;
    if (FAILED(g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&newPSO)))) {
        if (!isPassthrough) MessageBoxW(hwnd, L"Failed to create new Pipeline State Object.", L"D3D12 Error", MB_OK);
        return false;
    }
    
    if (isPassthrough) {
        g_passthroughPSO = newPSO;
    } else {
        g_dynamicPSO = newPSO;
        UpdateWindowTitle();
    }
    return true;
}

HRESULT InitializeSharing(UINT width, UINT height) {
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 1;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    g_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_sharedRtvHeap));
    
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&g_sharedSrvHeap));

    D3D12_HEAP_PROPERTIES heapProps = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width; texDesc.Height = height; texDesc.DepthOrArraySize = 1; texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    
    HRESULT hr = g_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_SHARED, &texDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_sharedTexture));
    if (FAILED(hr)) { LogHRESULT(L"Sharing: CreateCommittedResource for texture FAILED", hr); return hr; }
    
    g_device->CreateRenderTargetView(g_sharedTexture.Get(), nullptr, g_sharedRtvHeap->GetCPUDescriptorHandleForHeapStart());
    g_device->CreateShaderResourceView(g_sharedTexture.Get(), nullptr, g_sharedSrvHeap->GetCPUDescriptorHandleForHeapStart());
    
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, FALSE };
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd, NULL)) return E_FAIL;
    sa.lpSecurityDescriptor = sd;
    
    g_device->CreateSharedHandle(g_sharedTexture.Get(), &sa, GENERIC_ALL, g_sharedTextureName.c_str(), &g_sharedTextureHandle);
    g_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_sharedFence));
    g_device->CreateSharedHandle(g_sharedFence.Get(), &sa, GENERIC_ALL, g_sharedFenceName.c_str(), &g_sharedFenceHandle);
    
    std::wstring manifestName = L"D3D12_Producer_Manifest_" + std::to_wstring(GetCurrentProcessId());
    g_hManifest = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(BroadcastManifest), manifestName.c_str());
    if (sd) LocalFree(sd);
    if (!g_hManifest) { LogHRESULT(L"CreateFileMappingW failed", HRESULT_FROM_WIN32(GetLastError())); return E_FAIL; }
    
    g_pManifestView = (BroadcastManifest*)MapViewOfFile(g_hManifest, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    ZeroMemory(g_pManifestView, sizeof(BroadcastManifest));
    g_pManifestView->width = width; g_pManifestView->height = height;
    g_pManifestView->format = DXGI_FORMAT_B8G8R8A8_UNORM;
    g_pManifestView->adapterLuid = g_device->GetAdapterLuid();
    wcscpy_s(g_pManifestView->textureName, g_sharedTextureName.c_str());
    wcscpy_s(g_pManifestView->fenceName, g_sharedFenceName.c_str());
    Log(L"Sharing session initialized successfully.");
    return S_OK;
}

void FindAndConnectToProducer() {
    static auto lastSearchTime = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    if (g_isConnected) {
        HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, g_connectedProducerPid);
        if (hProcess == NULL || WaitForSingleObject(hProcess, 0) != WAIT_TIMEOUT) {
            DisconnectFromProducer();
        }
        if (hProcess) CloseHandle(hProcess);
        return;
    }
    if (std::chrono::steady_clock::now() - lastSearchTime < std::chrono::seconds(1)) return;
    lastSearchTime = std::chrono::steady_clock::now();

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return;
    
    PROCESSENTRY32W pe32 = {};
    pe32.dwSize = sizeof(PROCESSENTRY32W);
    DWORD selfPid = GetCurrentProcessId();

    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            if (pe32.th32ProcessID == selfPid) continue;
            
            const std::vector<std::wstring> manifestPrefixes = {L"D3D12_Producer_Manifest_", L"DirectPort_Producer_Manifest_"};
            for (const auto& prefix : manifestPrefixes) {
                std::wstring manifestName = prefix + std::to_wstring(pe32.th32ProcessID);
                g_hInputManifest = OpenFileMappingW(FILE_MAP_READ, FALSE, manifestName.c_str());
                if (g_hInputManifest) break;
            }

            if (!g_hInputManifest) continue;
            
            g_pInputManifestView = (BroadcastManifest*)MapViewOfFile(g_hInputManifest, FILE_MAP_READ, 0, 0, sizeof(BroadcastManifest));
            if (!g_pInputManifestView) { CloseHandle(g_hInputManifest); g_hInputManifest = nullptr; continue; }

            memcpy(&g_inputManifest, g_pInputManifestView, sizeof(BroadcastManifest));
            
            HANDLE hTexture = nullptr, hFence = nullptr;
            g_device->OpenSharedHandleByName(g_inputManifest.textureName, GENERIC_ALL, &hTexture);
            g_device->OpenSharedHandle(hTexture, IID_PPV_ARGS(&g_inputTexture));
            CloseHandle(hTexture);

            g_device->OpenSharedHandleByName(g_inputManifest.fenceName, GENERIC_ALL, &hFence);
            g_device->OpenSharedHandle(hFence, IID_PPV_ARGS(&g_inputFence));
            CloseHandle(hFence);

            if (g_inputTexture && g_inputFence) {
                D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
                srvHeapDesc.NumDescriptors = 1;
                srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                g_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&g_srvHeap));
                g_device->CreateShaderResourceView(g_inputTexture.Get(), nullptr, g_srvHeap->GetCPUDescriptorHandleForHeapStart());

                InitializeSharing(g_inputManifest.width, g_inputManifest.height);
                LoadShaderAndCreatePSO(nullptr, true);

                g_isConnected = true;
                g_connectedProducerPid = pe32.th32ProcessID;
                UpdateWindowTitle();
                CloseHandle(hSnapshot);
                return;
            }
            DisconnectFromProducer();
        } while (Process32NextW(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
}

void DisconnectFromProducer() {
    if (!g_isConnected) return;
    Log(L"Disconnecting from producer.");
    WaitForGpuIdle();
    
    g_isConnected = false;
    g_lastWaitedInputFrameValue = 0;
    g_connectedProducerPid = 0;
    
    if (g_pInputManifestView) UnmapViewOfFile(g_pInputManifestView);
    if (g_hInputManifest) CloseHandle(g_hInputManifest);
    g_pInputManifestView = nullptr;
    g_hInputManifest = nullptr;
    
    g_inputTexture.Reset();
    g_inputFence.Reset();
    g_srvHeap.Reset();
    g_passthroughPSO.Reset();
    g_dynamicPSO.Reset();

    if (g_pManifestView) UnmapViewOfFile(g_pManifestView);
    if (g_hManifest) CloseHandle(g_hManifest);
    if (g_sharedFenceHandle) CloseHandle(g_sharedFenceHandle);
    if (g_sharedTextureHandle) CloseHandle(g_sharedTextureHandle);
    g_pManifestView = nullptr;
    g_hManifest = nullptr;
    g_sharedFenceHandle = nullptr;
    g_sharedTextureHandle = nullptr;
    g_sharedFence.Reset();
    g_sharedTexture.Reset();
    g_sharedRtvHeap.Reset();
    g_sharedSrvHeap.Reset();
    
    UpdateWindowTitle();
}

void UpdateWindowTitle() {
    HWND hwnd = FindWindowW(L"DirectPortShaderFilterD3D12Wnd", NULL);
    if (!hwnd) return;

    WCHAR title[512];
    if (!g_isConnected) {
        wsprintfW(title, L"Shader Filter (PID: %lu) - Searching...", GetCurrentProcessId());
    } else {
        if (g_dynamicPSO) {
             wsprintfW(title, L"Shader Filter (PID: %lu) -> Source (PID: %lu) [FILTERING]", GetCurrentProcessId(), g_connectedProducerPid);
        } else {
             wsprintfW(title, L"Shader Filter (PID: %lu) -> Source (PID: %lu) [PASSTHROUGH]", GetCurrentProcessId(), g_connectedProducerPid);
        }
    }
    SetWindowTextW(hwnd, title);
}

void Cleanup() {
    WaitForGpuIdle();
    DisconnectFromProducer();
    CloseHandle(g_fenceEvent);
    if(g_pCbvDataBegin) g_constantBuffer->Unmap(0, nullptr);
}

void MoveToNextFrame() {
    const UINT64 currentFenceValue = g_fenceValue;
    g_commandQueue->Signal(g_renderFence.Get(), currentFenceValue);
    g_renderFenceValues[g_frameIndex] = currentFenceValue;
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
void OnResize(UINT width, UINT height) {
    if (!g_swapChain) return;
    WaitForGpuIdle();
    for (UINT i = 0; i < kFrameCount; i++) {
        g_renderTargets[i].Reset();
        g_renderFenceValues[i] = g_renderFenceValues[g_frameIndex];
    }
    g_swapChain->ResizeBuffers(kFrameCount, width, height, DXGI_FORMAT_UNKNOWN, 0);
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; i++) {
        g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_renderTargets[i]));
        g_device->CreateRenderTargetView(g_renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += g_rtvDescriptorSize;
    }
}