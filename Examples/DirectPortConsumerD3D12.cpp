#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "d3d12.h"
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl.h>
#include <string>
#include <chrono>
#include <tlhelp32.h>
#include <algorithm>
#include <cwctype>
#include <vector>
#include <intrin.h>
#include "resource.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "Synchronization.lib")

using namespace Microsoft::WRL;

void Log(const std::wstring& msg) {
    WCHAR buffer[1024];
    DWORD pid = GetCurrentProcessId();
    wsprintfW(buffer, L"[PID:%lu][ConsumerD3D12] %s\n", pid, msg.c_str());
    OutputDebugStringW(buffer);
}

struct BroadcastManifest {
    UINT64 frameValue;
    UINT width;
    UINT height;
    DXGI_FORMAT format;
    LUID adapterLuid;
    WCHAR textureName[256];
    WCHAR fenceName[256];
};

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
static ComPtr<ID3D12PipelineState>    g_pipelineState;
static UINT                           g_rtvDescriptorSize;
static UINT                           g_srvDescriptorSize;
static UINT                           g_frameIndex;
static ComPtr<ID3D12Fence>            g_renderFence;
static UINT64                         g_fenceValue;
static UINT64                         g_frameFenceValues[kFrameCount];
static HANDLE                         g_fenceEvent;
static LUID                           g_adapterLuid;
static HWND                           g_hwnd = nullptr;

const int MAX_PRODUCERS = 1;
struct ProducerConnection {
    bool                           isConnected = false;
    DWORD                          producerPid = 0;
    HANDLE                         hManifest = nullptr;
    BroadcastManifest*             pManifestView = nullptr;
    ComPtr<ID3D12Resource>         sharedTexture;
    ComPtr<ID3D12Fence>            sharedFence;
    UINT64                         lastSeenFrame = 0;
    UINT64                         frameToProcess = 0;
    ComPtr<ID3D12Resource>         privateTexture;
    UINT                           srvDescriptorIndex = 0;
    UINT                           connectedTextureWidth = 0;
    UINT                           connectedTextureHeight = 0;
    DXGI_FORMAT                    connectedTextureFormat = DXGI_FORMAT_UNKNOWN;
    std::wstring                   producerType;
};
static ProducerConnection g_producers[MAX_PRODUCERS];

void InitD3D12(HWND hwnd);
void LoadAssets();
void PopulateCommandList();
void FindAndConnectToProducers();
void DisconnectFromProducer(int i);
void WaitForGpu();
void Cleanup();
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void CreatePrivateTextureAndSRV(int producerIndex, UINT width, UINT height, DXGI_FORMAT format);
void UpdateWindowTitle();
HANDLE GetHandleFromName_D3D12(const WCHAR* name);
// --- FIX: Add forward declarations for resize handling ---
void OnResize(UINT width, UINT height);
void WaitForGpu();

const char* g_shaderHLSL = R"(
    Texture2D g_texture : register(t0);
    SamplerState g_sampler : register(s0);
    struct PSInput { float4 position : SV_POSITION; float2 uv : TEXCOORD; };
    PSInput VSMain(uint id : SV_VertexID) {
        PSInput output; float2 uv = float2((id << 1) & 2, id & 2);
        output.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1);
        output.uv = uv; return output;
    }
    float4 PSMain(PSInput input) : SV_TARGET {
        return g_texture.Sample(g_sampler, input.uv);
    }
)";

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    const WCHAR szClassName[] = L"D3D12ConsumerWindowClass";
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEXW) };
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.lpszClassName = szClassName;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(DPD_ICON1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    wcex.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(DPD_ICON1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    RegisterClassExW(&wcex);

    g_hwnd = CreateWindowExW(0, szClassName, L"DirectPort Consumer (D3D12) - Searching...", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, nullptr, nullptr, hInstance, nullptr);

    if (!g_hwnd) return 1;

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
            FindAndConnectToProducers();

            for (int i = 0; i < MAX_PRODUCERS; ++i) {
                auto& producer = g_producers[i];
                if (producer.isConnected) {
                    UINT64 capturedLastSeenFrame = producer.lastSeenFrame;
                    WaitOnAddress(&producer.pManifestView->frameValue, &capturedLastSeenFrame, sizeof(UINT64), 16);
                }
            }

            PopulateCommandList();
            ID3D12CommandList* ppCommandLists[] = { g_commandList.Get() };
            g_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
            g_swapChain->Present(1, 0);

            WaitForGpu();
            
            for (int i = 0; i < MAX_PRODUCERS; ++i) {
                auto& producer = g_producers[i];
                if (producer.isConnected && producer.frameToProcess > producer.lastSeenFrame) {
                    producer.lastSeenFrame = producer.frameToProcess;
                }
            }
        }
    }
    Cleanup();
    return static_cast<int>(msg.wParam);
}

// --- FIX: Add WndProc to handle WM_SIZE ---
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
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

void InitD3D12(HWND hwnd) {
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debugController0;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController0)))) {
        debugController0->EnableDebugLayer();
        ComPtr<ID3D12Debug1> debugController1;
        if (SUCCEEDED(debugController0->QueryInterface(IID_PPV_ARGS(&debugController1)))) {
            debugController1->SetEnableGPUBasedValidation(TRUE);
        }
    }
#endif

    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    
    ComPtr<IDXGIAdapter1> hardwareAdapter;
    for (UINT adapterIndex = 0; DXGI_ERROR_NOT_FOUND != factory->EnumAdapters1(adapterIndex, &hardwareAdapter); ++adapterIndex) {
        DXGI_ADAPTER_DESC1 desc;
        hardwareAdapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        g_adapterLuid = desc.AdapterLuid;
        break;
    }

    D3D12CreateDevice(hardwareAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device));

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_commandQueue));

    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    UINT width = clientRect.right - clientRect.left;
    UINT height = clientRect.bottom - clientRect.top;

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = kFrameCount;
    swapChainDesc.Width = width;
    swapChainDesc.Height = height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
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
    
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = MAX_PRODUCERS;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&g_srvHeap));
    g_srvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (UINT n = 0; n < kFrameCount; n++)
        g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_commandAllocators[n]));
    
    g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_commandAllocators[g_frameIndex].Get(), nullptr, IID_PPV_ARGS(&g_commandList));
    g_commandList->Close();

    g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_renderFence));
    g_fenceValue = 1;
    for(UINT n=0; n<kFrameCount; ++n) g_frameFenceValues[n] = 0;
    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

void LoadAssets() {
    D3D12_DESCRIPTOR_RANGE ranges[1] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParameters[1] = {};
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[0].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[0].DescriptorTable.pDescriptorRanges = ranges;

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

    ComPtr<ID3DBlob> vertexShader, pixelShader;
    D3DCompile(g_shaderHLSL, strlen(g_shaderHLSL), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vertexShader, &error);
    D3DCompile(g_shaderHLSL, strlen(g_shaderHLSL), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &pixelShader, &error);
    
    D3D12_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerDesc.CullMode = D3D12_CULL_MODE_BACK;
    rasterizerDesc.FrontCounterClockwise = FALSE;
    rasterizerDesc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rasterizerDesc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rasterizerDesc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    rasterizerDesc.DepthClipEnable = TRUE;
    rasterizerDesc.MultisampleEnable = FALSE;
    rasterizerDesc.AntialiasedLineEnable = FALSE;
    rasterizerDesc.ForcedSampleCount = 0;
    rasterizerDesc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_BLEND_DESC blendDesc = {};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    const D3D12_RENDER_TARGET_BLEND_DESC defaultRenderTargetBlendDesc = {
        FALSE,FALSE,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_LOGIC_OP_NOOP,
        D3D12_COLOR_WRITE_ENABLE_ALL,
    };
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        blendDesc.RenderTarget[i] = defaultRenderTargetBlendDesc;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = g_rootSignature.Get();
    psoDesc.VS.pShaderBytecode = vertexShader->GetBufferPointer();
    psoDesc.VS.BytecodeLength = vertexShader->GetBufferSize();
    psoDesc.PS.pShaderBytecode = pixelShader->GetBufferPointer();
    psoDesc.PS.BytecodeLength = pixelShader->GetBufferSize();
    psoDesc.RasterizerState = rasterizerDesc;
    psoDesc.BlendState = blendDesc;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_pipelineState));
}

void CreatePrivateTextureAndSRV(int producerIndex, UINT width, UINT height, DXGI_FORMAT format) {
    if (producerIndex < 0 || producerIndex >= MAX_PRODUCERS) return;
    
    auto& producer = g_producers[producerIndex];
    producer.privateTexture.Reset();

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    
    D3D12_HEAP_PROPERTIES heapProps = {D3D12_HEAP_TYPE_DEFAULT};
    if (SUCCEEDED(g_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&producer.privateTexture)))) {
        producer.srvDescriptorIndex = producerIndex;
        D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
        srvHandle.ptr += (producer.srvDescriptorIndex * g_srvDescriptorSize);
        
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        g_device->CreateShaderResourceView(producer.privateTexture.Get(), &srvDesc, srvHandle);
    } else {
        Log(L"Failed to create private texture for producer " + std::to_wstring(producerIndex));
    }
}

void PopulateCommandList() {
    g_commandAllocators[g_frameIndex]->Reset();
    g_commandList->Reset(g_commandAllocators[g_frameIndex].Get(), g_pipelineState.Get());

    g_commandList->SetGraphicsRootSignature(g_rootSignature.Get());
    ID3D12DescriptorHeap* ppHeaps[] = { g_srvHeap.Get() };
    g_commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = g_renderTargets[g_frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += (g_frameIndex * g_rtvDescriptorSize);
    g_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
    g_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    g_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    RECT clientRect;
    GetClientRect(g_hwnd, &clientRect);
    float windowWidth = (float)(clientRect.right - clientRect.left);
    float windowHeight = (float)(clientRect.bottom - clientRect.top);

    for (int i = 0; i < MAX_PRODUCERS; ++i) {
        auto& producer = g_producers[i];
        if (!producer.isConnected) continue;

        UINT64 latestFrame = producer.pManifestView->frameValue;
        
        if (latestFrame > producer.lastSeenFrame) {
            g_commandQueue->Wait(producer.sharedFence.Get(), latestFrame);
            
            D3D12_RESOURCE_BARRIER barriers[2] = {};
            barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[0].Transition.pResource = producer.sharedTexture.Get();
            barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[1].Transition.pResource = producer.privateTexture.Get();
            barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            g_commandList->ResourceBarrier(2, barriers);
            
            g_commandList->CopyResource(producer.privateTexture.Get(), producer.sharedTexture.Get());
            
            barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            g_commandList->ResourceBarrier(2, barriers);
            
            producer.frameToProcess = latestFrame;
        }

        if (producer.privateTexture) {
            D3D12_VIEWPORT vp = { 0.0f, 0.0f, windowWidth, windowHeight, 0.0f, 1.0f };
            D3D12_RECT sr = { 0, 0, (LONG)windowWidth, (LONG)windowHeight };
            g_commandList->RSSetViewports(1, &vp);
            g_commandList->RSSetScissorRects(1, &sr);

            D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
            srvHandle.ptr += (producer.srvDescriptorIndex * g_srvDescriptorSize);
            g_commandList->SetGraphicsRootDescriptorTable(0, srvHandle);
            g_commandList->DrawInstanced(3, 1, 0, 0);
        }
    }
    
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_commandList->ResourceBarrier(1, &barrier);
    g_commandList->Close();
}

void FindAndConnectToProducers() {
    static auto lastSearchTime = std::chrono::steady_clock::now() - std::chrono::seconds(2);

    for (int i = 0; i < MAX_PRODUCERS; ++i) {
        auto& producer = g_producers[i];
        if (!producer.isConnected) continue;

        bool needsDisconnect = false;
        HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, producer.producerPid);
        if (hProcess == NULL || WaitForSingleObject(hProcess, 0) != WAIT_TIMEOUT) {
            Log(L"Producer " + std::to_wstring(i) + L" (PID: " + std::to_wstring(producer.producerPid) + L") has terminated.");
            needsDisconnect = true;
        }
        if (hProcess) CloseHandle(hProcess);

        if (!needsDisconnect && producer.pManifestView &&
           (producer.pManifestView->width != producer.connectedTextureWidth ||
            producer.pManifestView->height != producer.connectedTextureHeight ||
            producer.pManifestView->format != producer.connectedTextureFormat)) {
            Log(L"Producer " + std::to_wstring(i) + L" resolution or format changed. Reconnecting...");
            needsDisconnect = true;
        }

        if (needsDisconnect) {
            DisconnectFromProducer(i);
        }
    }

    if (std::chrono::steady_clock::now() - lastSearchTime < std::chrono::seconds(1)) return;
    lastSearchTime = std::chrono::steady_clock::now();

    int availableSlot = -1;
    for (int i = 0; i < MAX_PRODUCERS; ++i) { 
        if (!g_producers[i].isConnected) { 
            availableSlot = i; 
            break; 
        } 
    }
    if (availableSlot == -1) return;

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe32 = {};
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    const std::wstring D3D11_PRODUCER_MANIFEST_PREFIX = L"DirectPort_Producer_Manifest_";
    const std::wstring D3D12_PRODUCER_MANIFEST_PREFIX = L"D3D12_Producer_Manifest_";

    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            bool alreadyConnected = false;
            for (int i = 0; i < MAX_PRODUCERS; ++i) {
                if (g_producers[i].isConnected && g_producers[i].producerPid == pe32.th32ProcessID) {
                    alreadyConnected = true;
                    break;
                }
            }
            if (alreadyConnected) continue;
            
            std::wstring currentProducerType;
            std::wstring manifestName;
            HANDLE hManifest = nullptr;

            manifestName = D3D12_PRODUCER_MANIFEST_PREFIX + std::to_wstring(pe32.th32ProcessID);
            hManifest = OpenFileMappingW(FILE_MAP_READ, FALSE, manifestName.c_str());

            if (hManifest) {
                currentProducerType = L"D3D12 Producer";
            } else {
                manifestName = D3D11_PRODUCER_MANIFEST_PREFIX + std::to_wstring(pe32.th32ProcessID);
                hManifest = OpenFileMappingW(FILE_MAP_READ, FALSE, manifestName.c_str());

                if (hManifest) {
                    std::wstring exeFileName(pe32.szExeFile);
                    std::transform(exeFileName.begin(), exeFileName.end(), exeFileName.begin(), ::towlower);

                    if (exeFileName.find(L"directportcamera") != std::wstring::npos) {
                        currentProducerType = L"Camera";
                    } else if (exeFileName.find(L"directportmultiplexer") != std::wstring::npos) {
                        currentProducerType = L"Multiplexer";
                    } else if (exeFileName.find(L"directportproducerd3d11") != std::wstring::npos) {
                        currentProducerType = L"D3D11 Producer";
                    } else {
                        currentProducerType = L"Unknown D3D11 Producer";
                    }
                }
            }
            
            if (!hManifest) continue;

            BroadcastManifest* pManifestView = (BroadcastManifest*)MapViewOfFile(hManifest, FILE_MAP_READ, 0, 0, sizeof(BroadcastManifest));
            if (!pManifestView) { 
                CloseHandle(hManifest); 
                continue; 
            }

            if (memcmp(&pManifestView->adapterLuid, &g_adapterLuid, sizeof(LUID)) != 0) {
                Log(L"Warning: Skipping producer PID " + std::to_wstring(pe32.th32ProcessID) + L" due to LUID mismatch.");
                UnmapViewOfFile(pManifestView);
                CloseHandle(hManifest);
                continue;
            }
            
            {
                ComPtr<ID3D12Resource> tempTexture;
                ComPtr<ID3D12Fence> tempFence;
                HRESULT hr = S_OK;

                HANDLE hTexture = GetHandleFromName_D3D12(pManifestView->textureName);
                if (hTexture == NULL) {
                    Log(L"Failed to get handle for texture: " + std::wstring(pManifestView->textureName));
                    UnmapViewOfFile(pManifestView); CloseHandle(hManifest); continue;
                }
                hr = g_device->OpenSharedHandle(hTexture, IID_PPV_ARGS(&tempTexture));
                CloseHandle(hTexture);
                if (FAILED(hr)) { 
                    Log(L"Failed to open shared texture handle for producer PID " + std::to_wstring(pe32.th32ProcessID));
                    UnmapViewOfFile(pManifestView); 
                    CloseHandle(hManifest); 
                    continue; 
                }

                HANDLE hFence = GetHandleFromName_D3D12(pManifestView->fenceName);
                if (hFence == NULL) {
                    Log(L"Failed to get handle for fence: " + std::wstring(pManifestView->fenceName));
                    UnmapViewOfFile(pManifestView); CloseHandle(hManifest); continue;
                }
                hr = g_device->OpenSharedHandle(hFence, IID_PPV_ARGS(&tempFence));
                CloseHandle(hFence);
                if (FAILED(hr)) { 
                    Log(L"Failed to open shared fence handle for producer PID " + std::to_wstring(pe32.th32ProcessID));
                    UnmapViewOfFile(pManifestView); 
                    CloseHandle(hManifest); 
                    continue; 
                }
                
                if (SUCCEEDED(hr)) {
                    auto& producer = g_producers[availableSlot];
                    
                    producer.lastSeenFrame = (pManifestView->frameValue > 0) ? (pManifestView->frameValue - 1) : 0;
                    
                    g_commandQueue->Wait(tempFence.Get(), pManifestView->frameValue);
                    
                    producer.producerPid = pe32.th32ProcessID;
                    producer.hManifest = hManifest;
                    producer.pManifestView = pManifestView;
                    producer.sharedTexture = tempTexture;
                    producer.sharedFence = tempFence;
                    producer.producerType = currentProducerType;
                    
                    D3D12_RESOURCE_DESC desc = tempTexture->GetDesc();
                    producer.connectedTextureWidth = (UINT)desc.Width;
                    producer.connectedTextureHeight = (UINT)desc.Height;
                    producer.connectedTextureFormat = desc.Format;

                    CreatePrivateTextureAndSRV(availableSlot, producer.connectedTextureWidth, producer.connectedTextureHeight, producer.connectedTextureFormat);
                    
                    producer.isConnected = true;
                    Log(L"Slot " + std::to_wstring(availableSlot) + L": Connected to " + currentProducerType + L" (PID: " + std::to_wstring(pe32.th32ProcessID) + L")");
                    UpdateWindowTitle();

                    CloseHandle(hSnapshot);
                    return;
                }
            }
            UnmapViewOfFile(pManifestView);
            CloseHandle(hManifest);

        } while (Process32NextW(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
}

void DisconnectFromProducer(int i) {
    if (i < 0 || i >= MAX_PRODUCERS || !g_producers[i].isConnected) return;
    auto& p = g_producers[i];
    Log(L"Disconnecting producer in slot " + std::to_wstring(i) + L" (PID: " + std::to_wstring(p.producerPid) + L")");
    if (p.pManifestView) UnmapViewOfFile(p.pManifestView);
    if (p.hManifest) CloseHandle(p.hManifest);
    p = {};
    UpdateWindowTitle();
}

void WaitForGpu() {
    g_commandQueue->Signal(g_renderFence.Get(), g_fenceValue);
    g_frameFenceValues[g_frameIndex] = g_fenceValue;
    g_fenceValue++;
    
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
    
    if (g_renderFence->GetCompletedValue() < g_frameFenceValues[g_frameIndex])
    {
        g_renderFence->SetEventOnCompletion(g_frameFenceValues[g_frameIndex], g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }
}

void Cleanup() {
    WaitForGpu();
    WaitForGpu();

    for (int i=0; i < MAX_PRODUCERS; ++i) DisconnectFromProducer(i);
    CloseHandle(g_fenceEvent);
}

void UpdateWindowTitle() {
    std::wstring title = L"DirectPort Consumer (D3D12) - ";
    int connectedCount = 0;
    std::wstring details;

    for (int i = 0; i < MAX_PRODUCERS; ++i) {
        if (g_producers[i].isConnected) {
            connectedCount++;
            details += L"[" + g_producers[i].producerType + L" PID: " + std::to_wstring(g_producers[i].producerPid) + L"] ";
        }
    }
    
    if (connectedCount == 0) {
        title += L"Searching...";
    } else {
        title += L"Connected to " + std::to_wstring(connectedCount) + L" producer(s): " + details;
    }
    
    SetWindowTextW(g_hwnd, title.c_str());
}

HANDLE GetHandleFromName_D3D12(const WCHAR* name) {
    ComPtr<ID3D12Device> d3d12Device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12Device)))) {
        return NULL;
    }
    HANDLE handle = nullptr;
    d3d12Device->OpenSharedHandleByName(name, GENERIC_ALL, &handle);
    return handle;
}

// --- FIX: Add a proper OnResize function for the D3D12 Consumer ---
void OnResize(UINT width, UINT height) {
    if (!g_swapChain) return;

    // Wait for any in-flight GPU work to complete.
    WaitForGpu();

    // Release references to the existing back buffers.
    for (UINT i = 0; i < kFrameCount; i++) {
        g_renderTargets[i].Reset();
        g_frameFenceValues[i] = g_frameFenceValues[g_frameIndex];
    }
    
    // Resize the swap chain.
    HRESULT hr = g_swapChain->ResizeBuffers(kFrameCount, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        // Handle error appropriately, e.g., by logging or exiting.
        Log(L"Failed to resize swap chain buffers.");
        return;
    }
    
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();

    // Re-create the render target views for the new back buffers.
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; i++) {
        g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_renderTargets[i]));
        g_device->CreateRenderTargetView(g_renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += g_rtvDescriptorSize;
    }
}