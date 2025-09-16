// --- DirectPortShaderProducerD3D12.cpp ---
// A DirectPort producer that can load a D3D12 pixel shader from a .cso or .hlsl file
// and broadcast the rendered result as a shared texture.
// Press SPACE to open a file dialog.

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
void Log(const std::wstring& msg) { WCHAR b[512]; wsprintfW(b, L"[PID:%lu][ShaderProducerD3D12] %s\n", GetCurrentProcessId(), msg.c_str()); OutputDebugStringW(b); }
void LogHRESULT(const std::wstring& msg, HRESULT hr) { WCHAR b[512]; wsprintfW(b, L"[PID:%lu][ShaderProducerD3D12] %s - HRESULT: 0x%08X\n", GetCurrentProcessId(), msg.c_str(), hr); OutputDebugStringW(b); }

// --- Core Structs ---
struct BroadcastManifest {
    UINT64 frameValue; UINT width; UINT height; DXGI_FORMAT format;
    LUID adapterLuid; WCHAR textureName[256]; WCHAR fenceName[256];
};
struct ConstantBuffer { float bar_rect[4]; float resolution[2]; float time; float pad; };

// --- Globals ---
static const UINT kFrameCount = 2;
static ComPtr<ID3D12Device>           g_device;
static ComPtr<ID3D12CommandQueue>     g_commandQueue;
static ComPtr<IDXGISwapChain3>        g_swapChain;
static ComPtr<ID3D12Resource>         g_renderTargets[kFrameCount];
static ComPtr<ID3D12CommandAllocator> g_commandAllocators[kFrameCount];
static ComPtr<ID3D12GraphicsCommandList> g_commandList;
static ComPtr<ID3D12DescriptorHeap>   g_rtvHeap;
static ComPtr<ID3D12DescriptorHeap>   g_srvHeap;
static ComPtr<ID3D12RootSignature>    g_rootSignature;
static ComPtr<ID3D12PipelineState>    g_dynamicPSO;
static ComPtr<ID3D12PipelineState>    g_passthroughPSO;
static UINT                           g_rtvDescriptorSize;
static UINT                           g_srvDescriptorSize;
static UINT                           g_frameIndex;
static ComPtr<ID3D12Fence>            g_renderFence;
static UINT64                         g_renderFenceValues[kFrameCount] = {};
static HANDLE                         g_fenceEvent;
static UINT64                         g_fenceValue = 1;
static HWND                           g_hwnd;

static ComPtr<ID3D12Resource>         g_constantBuffer;
static void*                          g_pCbvDataBegin = nullptr;
static auto                           gStartTime = std::chrono::high_resolution_clock::now();
static float                          gTime = 0.0f;

// --- Sharing Globals ---
static ComPtr<ID3D12Resource>         g_sharedTexture;
static ComPtr<ID3D12DescriptorHeap>   g_sharedRtvHeap;
static ComPtr<ID3D12Fence>            g_sharedFence;
static UINT64                         g_sharedFrameValue = 0;
static HANDLE                         g_hManifest = nullptr;
static BroadcastManifest*             g_pManifestView = nullptr;
static std::wstring                   g_sharedTextureName, g_sharedFenceName;
static HANDLE                         g_sharedTextureHandle = nullptr;
static HANDLE                         g_sharedFenceHandle = nullptr;

// --- Shader Code ---
const char* g_VertexShaderHLSL = R"(
struct VOut { float4 pos : SV_Position; };
VOut main(uint vid : SV_VertexID) {
    float2 uv = float2((vid << 1) & 2, vid & 2);
    VOut o; o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1);
    return o;
}
)";
const char* g_PassthroughVertexShaderHLSL = R"(
    struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD; };
    VOut main(uint vid : SV_VertexID) {
        float2 uv = float2((vid << 1) & 2, vid & 2);
        VOut o;
        o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1);
        o.uv = uv;
        return o;
    }
)";
const char* g_PassthroughPixelShaderHLSL = R"(
    Texture2D g_texture : register(t0);
    SamplerState g_sampler : register(s0);
    struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
    float4 main(PSInput input) : SV_TARGET {
        return g_texture.Sample(g_sampler, input.uv);
    }
)";


// --- Forward Declarations ---
static void InitD3D12(HWND hwnd);
static void LoadAssets();
static HRESULT InitializeSharing(UINT width, UINT height);
static void MoveToNextFrame();
static void WaitForGpuIdle();
static void Cleanup();
static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static void OnResize(UINT width, UINT height);
static bool LoadShaderAndCreatePSO(HWND hwnd);
static void UpdateTime();
static void PopulateCommandList();

// --- Main and Window ---
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    const WCHAR szClassName[] = L"DirectPortShaderProducerD3D12Wnd";
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEXW) };
    wcex.lpfnWndProc = WndProc; wcex.hInstance = hInstance; wcex.lpszClassName = szClassName;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(DPD_ICON1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    wcex.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(DPD_ICON1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    RegisterClassExW(&wcex);
    
    DWORD pid = GetCurrentProcessId();
    g_sharedTextureName = L"Global\\DirectPortTexture_" + std::to_wstring(pid);
    g_sharedFenceName = L"Global\\DirectPortFence_" + std::to_wstring(pid);
    WCHAR title[256];
    wsprintfW(title, L"Shader Producer (D3D12) (PID: %lu) - Press SPACE to load shader", pid);

    RECT rc = { 0, 0, 1280, 720 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    g_hwnd = CreateWindowExW(0, szClassName, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInstance, nullptr);

    SendMessage(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)wcex.hIcon);
    SendMessage(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)wcex.hIconSm);

    InitD3D12(g_hwnd);
    LoadAssets();
    InitializeSharing(1280, 720);

    ShowWindow(g_hwnd, nCmdShow);

    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            UpdateTime();
            PopulateCommandList();
            ID3D12CommandList* ppCommandLists[] = { g_commandList.Get() };
            g_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
            
            g_commandQueue->Signal(g_sharedFence.Get(), ++g_sharedFrameValue);
            if (g_pManifestView) {
                 g_pManifestView->frameValue = g_sharedFrameValue;
                 WakeByAddressAll(&g_pManifestView->frameValue);
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
            if (wParam == VK_SPACE) {
                WaitForGpuIdle(); 
                LoadShaderAndCreatePSO(hwnd);
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


// --- Core D3D12 Logic ---

void PopulateCommandList() {
    g_commandAllocators[g_frameIndex]->Reset();
    
    ID3D12PipelineState* pso = g_dynamicPSO ? g_dynamicPSO.Get() : nullptr;
    g_commandList->Reset(g_commandAllocators[g_frameIndex].Get(), pso);

    g_commandList->SetGraphicsRootSignature(g_rootSignature.Get());
    g_commandList->SetGraphicsRootConstantBufferView(0, g_constantBuffer->GetGPUVirtualAddress());

    // --- PASS 1: RENDER TO SHARED TEXTURE ---
    D3D12_RESOURCE_DESC sharedDesc = g_sharedTexture->GetDesc();
    D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)sharedDesc.Width, (float)sharedDesc.Height, 0.0f, 1.0f };
    D3D12_RECT scissorRect = { 0, 0, (LONG)sharedDesc.Width, (LONG)sharedDesc.Height };
    g_commandList->RSSetViewports(1, &viewport);
    g_commandList->RSSetScissorRects(1, &scissorRect);
    g_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = g_sharedTexture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE sharedRtvHandle = g_sharedRtvHeap->GetCPUDescriptorHandleForHeapStart();
    g_commandList->OMSetRenderTargets(1, &sharedRtvHandle, FALSE, nullptr);
    const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
    g_commandList->ClearRenderTargetView(sharedRtvHandle, clearColor, 0, nullptr);
    
    if (pso) {
        g_commandList->DrawInstanced(3, 1, 0, 0);
    }
    
    // --- PASS 2: BLIT TO SWAP CHAIN FOR PREVIEW ---
    D3D12_RESOURCE_BARRIER barriers[2] = {};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = g_sharedTexture.Get();
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = g_renderTargets[g_frameIndex].Get();
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_commandList->ResourceBarrier(2, barriers);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += (g_frameIndex * g_rtvDescriptorSize);
    g_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    const float previewClear[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    g_commandList->ClearRenderTargetView(rtvHandle, previewClear, 0, nullptr);
    
    g_commandList->SetPipelineState(g_passthroughPSO.Get());
    ID3D12DescriptorHeap* ppHeaps[] = { g_srvHeap.Get() };
    g_commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
    g_commandList->SetGraphicsRootDescriptorTable(1, g_srvHeap->GetGPUDescriptorHandleForHeapStart());

    RECT clientRect;
    GetClientRect(g_hwnd, &clientRect);
    D3D12_VIEWPORT windowViewport = { 0.0f, 0.0f, (float)(clientRect.right - clientRect.left), (float)(clientRect.bottom - clientRect.top), 0.0f, 1.0f };
    D3D12_RECT windowScissorRect = { 0, 0, (LONG)windowViewport.Width, (LONG)windowViewport.Height };
    g_commandList->RSSetViewports(1, &windowViewport);
    g_commandList->RSSetScissorRects(1, &windowScissorRect);
    g_commandList->DrawInstanced(3, 1, 0, 0);

    // Transition resources back to their original states
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_commandList->ResourceBarrier(2, barriers);
    
    g_commandList->Close();
}

void UpdateTime() {
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

// --- Shader Loading ---

bool LoadShaderAndCreatePSO(HWND hwnd) {
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Shader Files (*.cso, *.hlsl)\0*.cso;*.hlsl\0All Files\0*.*\0";
    ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return false;

    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { MessageBoxW(hwnd, L"Could not open shader file.", L"File Error", MB_OK); return false; }
    DWORD size = GetFileSize(f, nullptr);
    if (size == INVALID_FILE_SIZE || size == 0) { CloseHandle(f);  MessageBoxW(hwnd, L"Shader file is empty or invalid.", L"File Error", MB_OK); return false; }
    std::vector<char> buffer(size);
    DWORD read = 0;
    ReadFile(f, buffer.data(), size, &read, nullptr);
    CloseHandle(f);

    ComPtr<ID3DBlob> psBlob;
    std::wstring ext = path;
    ext = ext.substr(ext.find_last_of(L".") + 1);
    if (_wcsicmp(ext.c_str(), L"hlsl") == 0) {
        ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3DCompile(buffer.data(), buffer.size(), nullptr, nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &psBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) MessageBoxA(hwnd, (char*)errorBlob->GetBufferPointer(), "HLSL Compile Error", MB_OK);
            return false;
        }
    } else {
        HRESULT hr = D3DCreateBlob(buffer.size(), &psBlob);
        if (FAILED(hr)) return false;
        memcpy(psBlob->GetBufferPointer(), buffer.data(), buffer.size());
    }

    ComPtr<ID3DBlob> vsBlob;
    D3DCompile(g_VertexShaderHLSL, strlen(g_VertexShaderHLSL), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, nullptr);
    D3D12_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
    rasterizerDesc.DepthClipEnable = TRUE;
    D3D12_BLEND_DESC blendDesc = {};
    const D3D12_RENDER_TARGET_BLEND_DESC defaultRenderTargetBlendDesc = { FALSE,FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL, };
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        blendDesc.RenderTarget[i] = defaultRenderTargetBlendDesc;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { nullptr, 0 };
    psoDesc.pRootSignature = g_rootSignature.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.RasterizerState = rasterizerDesc;
    psoDesc.BlendState = blendDesc;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> newPSO;
    if (FAILED(g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&newPSO)))) {
        MessageBoxW(hwnd, L"Failed to create new Pipeline State Object. The shader may be incompatible.", L"D3D12 Error", MB_OK);
        return false;
    }

    g_dynamicPSO = newPSO;
    std::wstring title = L"Shader Producer (D3D12) - Loaded: " + std::wstring(path);
    SetWindowTextW(hwnd, title.c_str());
    return true;
}


// --- Setup and Teardown ---
void InitD3D12(HWND hwnd) {
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
    }
#endif

    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));

    g_device.Reset();
    D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device));

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_commandQueue));

    RECT rc;
    GetClientRect(hwnd, &rc);

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
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    g_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_rtvHeap));
    g_rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT n = 0; n < kFrameCount; n++) {
        g_swapChain->GetBuffer(n, IID_PPV_ARGS(&g_renderTargets[n]));
        g_device->CreateRenderTargetView(g_renderTargets[n].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += (1 * g_rtvDescriptorSize);
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
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParameters[0].Descriptor = { 0, 0 };
    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[1].DescriptorTable = { 1, ranges };

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
    
    ComPtr<ID3DBlob> ptVS, ptPS;
    D3DCompile(g_PassthroughVertexShaderHLSL, strlen(g_PassthroughVertexShaderHLSL), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &ptVS, &error);
    D3DCompile(g_PassthroughPixelShaderHLSL, strlen(g_PassthroughPixelShaderHLSL), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &ptPS, &error);
    
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = g_rootSignature.Get();
    psoDesc.VS = { ptVS->GetBufferPointer(), ptVS->GetBufferSize() };
    psoDesc.PS = { ptPS->GetBufferPointer(), ptPS->GetBufferSize() };
    D3D12_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
    rasterizerDesc.DepthClipEnable = TRUE;
    psoDesc.RasterizerState = rasterizerDesc;
    D3D12_BLEND_DESC blendDesc = {};
    const D3D12_RENDER_TARGET_BLEND_DESC defaultRenderTargetBlendDesc = { FALSE,FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL, };
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        blendDesc.RenderTarget[i] = defaultRenderTargetBlendDesc;
    psoDesc.BlendState = blendDesc;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_passthroughPSO));

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

HRESULT InitializeSharing(UINT width, UINT height) {
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 1;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    g_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_sharedRtvHeap));
    
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&g_srvHeap));

    D3D12_HEAP_PROPERTIES heapProps = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width; texDesc.Height = height; texDesc.DepthOrArraySize = 1; texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    
    D3D12_CLEAR_VALUE optimizedClearValue = {};
    optimizedClearValue.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    optimizedClearValue.Color[0] = 0.0f; optimizedClearValue.Color[1] = 0.2f;
    optimizedClearValue.Color[2] = 0.4f; optimizedClearValue.Color[3] = 1.0f;
    
    HRESULT hr = g_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_SHARED, &texDesc, D3D12_RESOURCE_STATE_COMMON, &optimizedClearValue, IID_PPV_ARGS(&g_sharedTexture));
    if (FAILED(hr)) { LogHRESULT(L"Sharing: CreateCommittedResource for texture FAILED", hr); return hr; }
    
    g_device->CreateRenderTargetView(g_sharedTexture.Get(), nullptr, g_sharedRtvHeap->GetCPUDescriptorHandleForHeapStart());
    g_device->CreateShaderResourceView(g_sharedTexture.Get(), nullptr, g_srvHeap->GetCPUDescriptorHandleForHeapStart());

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
    g_pManifestView->width = width;
    g_pManifestView->height = height;
    g_pManifestView->format = DXGI_FORMAT_B8G8R8A8_UNORM;
    g_pManifestView->adapterLuid = g_device->GetAdapterLuid();
    wcscpy_s(g_pManifestView->textureName, g_sharedTextureName.c_str());
    wcscpy_s(g_pManifestView->fenceName, g_sharedFenceName.c_str());
    Log(L"Sharing session initialized successfully.");
    return S_OK;
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

void Cleanup() {
    WaitForGpuIdle();
    if (g_pManifestView) UnmapViewOfFile(g_pManifestView);
    if (g_hManifest) CloseHandle(g_hManifest);
    if (g_sharedFenceHandle) CloseHandle(g_sharedFenceHandle);
    if (g_sharedTextureHandle) CloseHandle(g_sharedTextureHandle);
    CloseHandle(g_fenceEvent);
    if(g_pCbvDataBegin) g_constantBuffer->Unmap(0, nullptr);
    g_pCbvDataBegin = nullptr;
}