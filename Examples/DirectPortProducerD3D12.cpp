// --- DirectPortProducerD3D12.cpp ---

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl.h>
#include <sddl.h>
#include <string>
#include <chrono>
#include <intrin.h>
#include "resource.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "Synchronization.lib")

using namespace Microsoft::WRL;

struct BroadcastManifest {
    UINT64 frameValue;
    UINT width;
    UINT height;
    DXGI_FORMAT format;
    LUID adapterLuid;
    WCHAR textureName[256];
    WCHAR fenceName[256];
};

void Log(const std::wstring& msg) {
    WCHAR buffer[1024];
    DWORD pid = GetCurrentProcessId();
    wsprintfW(buffer, L"[PID:%lu][D3D12_Producer] %s\n", pid, msg.c_str());
    OutputDebugStringW(buffer);
}
void LogHRESULT(const std::wstring& msg, HRESULT hr) { WCHAR b[512]; wsprintfW(b, L"[D3D12_Producer] %s - HRESULT: 0x%08X\n", msg.c_str(), hr); OutputDebugStringW(b); }


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
static ComPtr<ID3D12PipelineState>    g_passthroughPSO;
static UINT                           g_rtvDescriptorSize;
static UINT                           g_srvDescriptorSize;
static UINT                           g_frameIndex;
static ComPtr<ID3D12Fence>            g_renderFence;
static UINT64                         g_renderFenceValues[kFrameCount] = {};
static HWND                           g_hwnd;
static HANDLE                         g_fenceEvent;
static UINT64                         g_fenceValue = 1;

struct ConstantBuffer { float bar_rect[4]; float resolution[2]; };
static ComPtr<ID3D12Resource>         g_constantBuffer;
static UINT8*                         g_pCbvDataBegin = nullptr;
static float gBarPosX = 0.0f, gBarPosY = 0.0f;
static float gBarVelX = 0.2f, gBarVelY = 0.3f;
static auto gLastFrameTime = std::chrono::high_resolution_clock::now();

static ComPtr<ID3D12Resource>         g_sharedTexture;
static ComPtr<ID3D12DescriptorHeap>   g_sharedRtvHeap;
static ComPtr<ID3D12Fence>            g_sharedFence;
static UINT64                         g_sharedFrameValue = 0;

static HANDLE                         g_hManifest = nullptr;
static BroadcastManifest*           g_pManifestView = nullptr;
static std::wstring                   g_sharedTextureName, g_sharedFenceName;
static HANDLE                         g_sharedTextureHandle = nullptr;
static HANDLE                         g_sharedFenceHandle = nullptr;


void InitD3D12(HWND hwnd);
void LoadAssets();
void PopulateCommandList();
void MoveToNextFrame();
void UpdateAnimation(float dt);
HRESULT InitializeSharing(UINT width, UINT height);
void ShutdownSharing();
void Cleanup();
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void WaitForGpuIdle();
void OnResize(UINT width, UINT height);

const char* g_GenerationShaderHLSL = R"(
    cbuffer Constants : register(b0) { float4 bar_rect; float2 resolution; };
    struct PSInput { float4 position : SV_POSITION; };
    float3 SmpteBar75(float u) {
        if      (u < 1.0/7.0) return float3(0.75, 0.75, 0.75);
        else if (u < 2.0/7.0) return float3(0.75, 0.75, 0.00);
        else if (u < 3.0/7.0) return float3(0.00, 0.75, 0.75);
        else if (u < 4.0/7.0) return float3(0.00, 0.75, 0.00);
        else if (u < 5.0/7.0) return float3(0.75, 0.00, 0.75);
        else if (u < 6.0/7.0) return float3(0.75, 0.00, 0.00);
        else                  return float3(0.00, 0.00, 0.75);
    }
    PSInput VSMain(uint id : SV_VertexID) {
        PSInput output; float2 uv = float2((id << 1) & 2, id & 2);
        output.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1); return output;
    }
    float4 PSMain(PSInput input) : SV_TARGET {
        float2 ndc = input.position.xy / resolution.xy * float2(2.0, -2.0) + float2(-1.0, 1.0);
        float3 bg = lerp(float3(0.01, 0.01, 0.03), float3(0.03, 0.02, 0.06), input.position.y / resolution.y);
        bg *= (1.0 - 0.55 * smoothstep(0.55, 1.10, length(ndc)));
        float xL = bar_rect.x; float yT = bar_rect.y; float w = bar_rect.z; float h = bar_rect.w;
        if (ndc.x < xL || ndc.x > (xL + w) || ndc.y > yT || ndc.y < (yT - h)) {
             return float4(bg, 1.0);
        }
        float u = (ndc.x - xL) / w;
        float v = (yT - ndc.y) / h;
        float3 color = float3(0,0,0);
        if (v < (2.0/3.0)) {
            color = SmpteBar75(u);
        }
        else if (v < 0.75) {
            if      (u < 1.0/7.0) color = float3(0.0, 0.0, 0.75);
            else if (u < 2.0/7.0) color = float3(0.0, 0.0, 0.0);
            else if (u < 3.0/7.0) color = float3(0.75, 0.0, 0.75);
            else if (u < 4.0/7.0) color = float3(0.0, 0.0, 0.0);
            else if (u < 5.0/7.0) color = float3(0.0, 0.75, 0.75);
            else if (u < 6.0/7.0) color = float3(0.0, 0.0, 0.0);
            else                  color = float3(0.75, 0.75, 0.75);
        }
        else {
            if (u < 5.0/7.0) {
                if      (u < 2.5/7.0) color = float3(0.0, 0.0, 0.0);
                else if (u < 3.5/7.0) color = float3(1.0, 1.0, 1.0);
                else                  color = float3(0.0, 0.0, 0.0);
            } else {
                float pluge_u = (u - 5.0/7.0) / (2.0/7.0);
                if      (pluge_u < 1.0/3.0) color = float3(0.03, 0.03, 0.03);
                else if (pluge_u < 2.0/3.0) color = float3(0.075, 0.075, 0.075);
                else                        color = float3(0.115, 0.115, 0.115);
            }
        }
        return float4(color, 1.0);
    }
)";

const char* g_PassthroughShaderHLSL = R"(
    Texture2D g_texture : register(t0);
    SamplerState g_sampler : register(s0);
    struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };

    PSInput VSMain(uint id : SV_VertexID) {
        PSInput output;
        float2 uv = float2((id << 1) & 2, id & 2);
        output.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1);
        output.uv = uv;
        return output;
    }

    float4 PSMain(PSInput input) : SV_TARGET {
        return g_texture.Sample(g_sampler, input.uv);
    }
)";

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    const WCHAR szClassName[] = L"DirectPortProducerD3D12WindowClass";
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEXW) };
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.lpszClassName = szClassName;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(DPD_ICON1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    wcex.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(DPD_ICON1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    RegisterClassExW(&wcex);
    
    DWORD pid = GetCurrentProcessId();
    g_sharedTextureName = L"Global\\DirectPortTexture_" + std::to_wstring(pid);
    g_sharedFenceName = L"Global\\DirectPortFence_" + std::to_wstring(pid);
    WCHAR title[256];
    wsprintfW(title, L"DirectPort Producer (D3D12) (PID: %lu)", pid);

    RECT rc = { 0, 0, 1280, 720 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    g_hwnd = CreateWindowExW(0, szClassName, title, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInstance, nullptr);

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
            UpdateAnimation(std::chrono::duration<float>(std::chrono::high_resolution_clock::now() - gLastFrameTime).count());
            gLastFrameTime = std::chrono::high_resolution_clock::now();
            
            PopulateCommandList();

            ID3D12CommandList* ppCommandLists[] = { g_commandList.Get() };
            g_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
            
            if (g_sharedFence) {
                g_commandQueue->Signal(g_sharedFence.Get(), ++g_sharedFrameValue);
            }
            
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

void InitD3D12(HWND hwnd) {
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
    }

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
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
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
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[0].Descriptor = { 0, 0 }; // CBV at b0
    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[1].DescriptorTable = { 1, ranges }; // SRV at t0

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

    ComPtr<ID3DBlob> vertexShader, genPixelShader, ptPixelShader, passthroughVS;
    D3DCompile(g_GenerationShaderHLSL, strlen(g_GenerationShaderHLSL), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vertexShader, &error);
    D3DCompile(g_GenerationShaderHLSL, strlen(g_GenerationShaderHLSL), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &genPixelShader, &error);
    D3DCompile(g_PassthroughShaderHLSL, strlen(g_PassthroughShaderHLSL), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &passthroughVS, &error);
    D3DCompile(g_PassthroughShaderHLSL, strlen(g_PassthroughShaderHLSL), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &ptPixelShader, &error);

    D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
    rasterizerDesc.FrontCounterClockwise = FALSE;
    rasterizerDesc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rasterizerDesc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rasterizerDesc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    rasterizerDesc.DepthClipEnable = TRUE;

    D3D12_BLEND_DESC blendDesc = {};
    const D3D12_RENDER_TARGET_BLEND_DESC defaultRenderTargetBlendDesc = { FALSE,FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL, };
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) blendDesc.RenderTarget[i] = defaultRenderTargetBlendDesc;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = g_rootSignature.Get();
    psoDesc.RasterizerState = rasterizerDesc;
    psoDesc.BlendState = blendDesc;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    
    // Generation PSO
    psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
    psoDesc.PS = { genPixelShader->GetBufferPointer(), genPixelShader->GetBufferSize() };
    psoDesc.InputLayout = { nullptr, 0 }; // No input from CPU for generation
    g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_pipelineState));

    // Passthrough PSO
    psoDesc.VS = { passthroughVS->GetBufferPointer(), passthroughVS->GetBufferSize() };
    psoDesc.PS = { ptPixelShader->GetBufferPointer(), ptPixelShader->GetBufferSize() };
    psoDesc.InputLayout = { nullptr, 0 }; // No input from CPU for passthrough
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
    g_constantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&g_pCbvDataBegin));
}

HRESULT InitializeSharing(UINT width, UINT height) {
    if (width == 0 || height == 0) return E_INVALIDARG;
    
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 1;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    g_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_sharedRtvHeap));

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&g_srvHeap));
    g_srvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    
    D3D12_HEAP_PROPERTIES heapProps = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    D3D12_CLEAR_VALUE optimizedClearValue = {};
    optimizedClearValue.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    
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
    
    ComPtr<IDXGIFactory4> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        ComPtr<IDXGIAdapter1> adapter;
        LUID deviceLuid = g_device->GetAdapterLuid();
        for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &adapter)); ++i) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if (memcmp(&desc.AdapterLuid, &deviceLuid, sizeof(LUID)) == 0) {
                std::wstring manifestName = L"DirectPort_Producer_Manifest_" + std::to_wstring(GetCurrentProcessId());
                g_hManifest = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(BroadcastManifest), manifestName.c_str());
                if (sd) LocalFree(sd);
                if (!g_hManifest) { LogHRESULT(L"CreateFileMappingW failed", HRESULT_FROM_WIN32(GetLastError())); return E_FAIL; }

                g_pManifestView = (BroadcastManifest*)MapViewOfFile(g_hManifest, FILE_MAP_ALL_ACCESS, 0, 0, 0);
                ZeroMemory(g_pManifestView, sizeof(BroadcastManifest));
                g_pManifestView->width = width;
                g_pManifestView->height = height;
                g_pManifestView->format = DXGI_FORMAT_B8G8R8A8_UNORM;
                g_pManifestView->adapterLuid = desc.AdapterLuid;
                wcscpy_s(g_pManifestView->textureName, g_sharedTextureName.c_str());
                wcscpy_s(g_pManifestView->fenceName, g_sharedFenceName.c_str());
                Log(L"Sharing session initialized successfully.");
                return S_OK;
            }
        }
    }
    if (sd) LocalFree(sd);
    return E_FAIL;
}

void ShutdownSharing() {
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
    Log(L"Sharing session shut down.");
}

void UpdateConstantBuffer(float width, float height) {
    const float BAR_BASE_WIDTH_NDC = 1.0f;
    const float BAR_ASPECT_RATIO = 16.0f / 10.0f;
    const float BAR_HEIGHT_NDC = BAR_BASE_WIDTH_NDC / BAR_ASPECT_RATIO * (width / height);

    ConstantBuffer cb;
    cb.resolution[0] = width;
    cb.resolution[1] = height;
    cb.bar_rect[0] = gBarPosX - BAR_BASE_WIDTH_NDC / 2.f;
    cb.bar_rect[1] = gBarPosY + BAR_HEIGHT_NDC / 2.f;
    cb.bar_rect[2] = BAR_BASE_WIDTH_NDC;
    cb.bar_rect[3] = BAR_HEIGHT_NDC;
    memcpy(g_pCbvDataBegin, &cb, sizeof(ConstantBuffer));
}

void UpdateAnimation(float dt) {
    D3D12_RESOURCE_DESC desc = g_sharedTexture->GetDesc();
    const float RENDER_W = (float)desc.Width;
    const float RENDER_H = (float)desc.Height;
    const float BAR_BASE_WIDTH_NDC = 1.0f;
    const float BAR_ASPECT_RATIO = 16.0f / 10.0f;
    const float BAR_HEIGHT_NDC = BAR_BASE_WIDTH_NDC / BAR_ASPECT_RATIO * (RENDER_W / RENDER_H);

    gBarPosX += gBarVelX * dt; gBarPosY += gBarVelY * dt;
    if (abs(gBarPosX) > (1.0f - BAR_BASE_WIDTH_NDC / 2.f)) { gBarVelX *= -1.0f; gBarPosX = (1.0f - BAR_BASE_WIDTH_NDC / 2.f) * (gBarPosX > 0 ? 1 : -1); }
    if (abs(gBarPosY) > (1.0f - BAR_HEIGHT_NDC / 2.f)) { gBarVelY *= -1.0f; gBarPosY = (1.0f - BAR_HEIGHT_NDC / 2.f) * (gBarPosY > 0 ? 1 : -1); }
}

void PopulateCommandList() {
    g_commandAllocators[g_frameIndex]->Reset();
    g_commandList->Reset(g_commandAllocators[g_frameIndex].Get(), g_pipelineState.Get());

    g_commandList->SetGraphicsRootSignature(g_rootSignature.Get());
    g_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // --- PASS 1: Render scene to the Shared Texture (Fixed Size) ---
    D3D12_RESOURCE_DESC sharedDesc = g_sharedTexture->GetDesc();
    UpdateConstantBuffer((float)sharedDesc.Width, (float)sharedDesc.Height);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = g_sharedTexture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE sharedRtvHandle = g_sharedRtvHeap->GetCPUDescriptorHandleForHeapStart();
    g_commandList->OMSetRenderTargets(1, &sharedRtvHandle, FALSE, nullptr);
    
    D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)sharedDesc.Width, (float)sharedDesc.Height, 0.0f, 1.0f };
    D3D12_RECT scissorRect = { 0, 0, (LONG)sharedDesc.Width, (LONG)sharedDesc.Height };
    g_commandList->RSSetViewports(1, &viewport);
    g_commandList->RSSetScissorRects(1, &scissorRect);
    g_commandList->SetGraphicsRootConstantBufferView(0, g_constantBuffer->GetGPUVirtualAddress());
    g_commandList->DrawInstanced(3, 1, 0, 0);

    // --- PASS 2: Blit the Shared Texture to the Window's Back Buffer for Preview ---
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
    
    HRESULT hr = g_swapChain->ResizeBuffers(kFrameCount, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        LogHRESULT(L"Swap chain resize failed", hr);
        return;
    }
    
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
    ShutdownSharing();
    CloseHandle(g_fenceEvent);
    if(g_pCbvDataBegin) g_constantBuffer->Unmap(0, nullptr);
    g_pCbvDataBegin = nullptr;
}

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