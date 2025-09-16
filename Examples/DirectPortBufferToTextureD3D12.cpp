#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl.h>
#include <sddl.h>
#include <string>
#include <iostream>
#include <vector>
#include <synchapi.h>
#include <shellapi.h>
#include <tlhelp32.h> // Added for process enumeration
#include <chrono>   // Added for search timer

#pragma comment(lib, "Synchronization.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "advapi32.lib")

using namespace Microsoft::WRL;

struct SharedBufferManifest {
    UINT64 frameValue; UINT64 bufferSize; LUID adapterLuid;
    WCHAR resourceName[256]; WCHAR fenceName[256];
};
struct BroadcastManifest {
    UINT64 frameValue; UINT width; UINT height; DXGI_FORMAT format;
    LUID adapterLuid; WCHAR textureName[256]; WCHAR fenceName[256];
};

const char* g_vertexShaderHLSL = R"(
    struct PSInput { float4 pos : SV_POSITION; };
    PSInput VSMain(uint id : SV_VertexID) {
        PSInput result;
        float2 uv = float2((id << 1) & 2, id & 2);
        result.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1);
        return result;
    }
)";
const char* g_pixelShaderHLSL = R"(
    cbuffer ShaderConstants : register(b0) {
        float data;
    };
    float4 PSMain(float4 pos : SV_POSITION) : SV_TARGET {
        float r = sin(data * 0.5) * 0.5 + 0.5;
        float g = cos(data * 0.3) * 0.5 + 0.5;
        float b = sin(data * 0.8) * 0.5 + 0.5;
        return float4(r, g, b, 1.0);
    }
)";

HWND g_hwnd = nullptr;
const UINT g_frameCount = 2;
UINT g_frameIndex;
ComPtr<IDXGISwapChain3> g_swapChain;
ComPtr<ID3D12Resource> g_renderTargets[g_frameCount];
ComPtr<ID3D12DescriptorHeap> g_rtvHeap;
UINT g_rtvDescriptorSize;
ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12CommandQueue> g_commandQueue;
ComPtr<ID3D12CommandAllocator> g_commandAllocator;
ComPtr<ID3D12GraphicsCommandList> g_commandList;
HANDLE g_fenceEvent;
ComPtr<ID3D12Fence> g_fence;
UINT64 g_fenceValue;
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void WaitForPreviousFrame();
void PopulateCommandList(ID3D12PipelineState* pso, ID3D12RootSignature* rootSig, ID3D12Resource* cb, ID3D12Resource* privateTex, ID3D12Resource* sharedTex, ComPtr<ID3D12DescriptorHeap> rtvHeap, const D3D12_CPU_DESCRIPTOR_HANDLE& rtvHandle, UINT texWidth, UINT texHeight);
void UpdateWindowTitle();
void DisconnectFromProducer();
void FindAndConnectToProducer();

// --- State Globals ---
static bool g_isConnected = false;
static DWORD g_producerPid = 0;
static ComPtr<ID3D12Resource> g_sharedBufferIn;
static ComPtr<ID3D12Fence> g_sharedFenceIn;
static UINT64 g_lastSeenFrame = 0;

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int nCmdShow) {
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance, NULL, NULL, NULL, NULL, L"BufferToTextureD3D12", NULL };
    RegisterClassExW(&wc);
    g_hwnd = CreateWindowW(wc.lpszClassName, L"Buffer To Texture D3D12", WS_OVERLAPPEDWINDOW, 100, 100, 800, 600, NULL, NULL, hInstance, NULL);
    UpdateWindowTitle();

    D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device));
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    g_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_commandQueue));
    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = g_frameCount;
    swapChainDesc.Width = 800; swapChainDesc.Height = 600;
    swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;
    ComPtr<IDXGISwapChain1> swapChain;
    factory->CreateSwapChainForHwnd(g_commandQueue.Get(), g_hwnd, &swapChainDesc, nullptr, nullptr, &swapChain);
    swapChain.As(&g_swapChain);
    
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = g_frameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    g_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_rtvHeap));
    g_rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle(g_rtvHeap->GetCPUDescriptorHandleForHeapStart());
    for (UINT n = 0; n < g_frameCount; n++) {
        g_swapChain->GetBuffer(n, IID_PPV_ARGS(&g_renderTargets[n]));
        g_device->CreateRenderTargetView(g_renderTargets[n].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += g_rtvDescriptorSize;
    }
    
    g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_commandAllocator));
    g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_commandAllocator.Get(), nullptr, IID_PPV_ARGS(&g_commandList));
    g_commandList->Close();
    g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    g_fenceValue = 1;
    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();

    const UINT TEX_WIDTH = 1280, TEX_HEIGHT = 720;
    ComPtr<ID3D12Resource> privateTexture, sharedTexture;
    D3D12_HEAP_PROPERTIES defaultHeapProps = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.Width = TEX_WIDTH; texDesc.Height = TEX_HEIGHT; texDesc.DepthOrArraySize = 1; texDesc.MipLevels = 1;
    texDesc.SampleDesc.Count = 1; texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    g_device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&privateTexture));
    texDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    g_device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_SHARED, &texDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&sharedTexture));

    PSECURITY_DESCRIPTOR sd = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd, NULL);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd, FALSE };
    DWORD selfPid = GetCurrentProcessId();
    std::wstring resourceName = L"Global\\DirectPort_Texture_" + std::to_wstring(selfPid);
    std::wstring fenceName = L"Global\\DirectPort_Fence_" + std::to_wstring(selfPid);
    std::wstring manifestName = L"D3D12_Producer_Manifest_" + std::to_wstring(selfPid);

    HANDLE textureHandle = nullptr;
    g_device->CreateSharedHandle(sharedTexture.Get(), &sa, GENERIC_ALL, resourceName.c_str(), &textureHandle);
    ComPtr<ID3D12Fence> sharedTextureFence;
    g_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&sharedTextureFence));
    HANDLE textureFenceHandle = nullptr;
    g_device->CreateSharedHandle(sharedTextureFence.Get(), &sa, GENERIC_ALL, fenceName.c_str(), &textureFenceHandle);
    HANDLE hTexManifestOut = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(BroadcastManifest), manifestName.c_str());
    LocalFree(sd);
    BroadcastManifest* texManifestOutView = (BroadcastManifest*)MapViewOfFile(hTexManifestOut, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    
    texManifestOutView->frameValue = 0; texManifestOutView->width = TEX_WIDTH; texManifestOutView->height = TEX_HEIGHT;
    texManifestOutView->format = DXGI_FORMAT_B8G8R8A8_UNORM; texManifestOutView->adapterLuid = g_device->GetAdapterLuid();
    wcscpy_s(texManifestOutView->textureName, resourceName.c_str());
    wcscpy_s(texManifestOutView->fenceName, fenceName.c_str());

    ComPtr<ID3DBlob> vs, ps, errors;
    D3DCompile(g_vertexShaderHLSL, strlen(g_vertexShaderHLSL), nullptr, nullptr, nullptr, "VSMain", "vs_5_1", 0, 0, &vs, &errors);
    D3DCompile(g_pixelShaderHLSL, strlen(g_pixelShaderHLSL), nullptr, nullptr, nullptr, "PSMain", "ps_5_1", 0, 0, &ps, &errors);
    
    D3D12_ROOT_PARAMETER1 rootParameters[1] = {};
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[0].Descriptor.ShaderRegister = 0; rootParameters[0].Descriptor.RegisterSpace = 0;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rootSigDesc.Desc_1_1.NumParameters = _countof(rootParameters);
    rootSigDesc.Desc_1_1.pParameters = rootParameters;
    ComPtr<ID3DBlob> signature;
    D3D12SerializeVersionedRootSignature(&rootSigDesc, &signature, &errors);
    ComPtr<ID3D12RootSignature> rootSignature;
    g_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&rootSignature));
    
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = rootSignature.Get();
    psoDesc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    psoDesc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.DepthStencilState.DepthEnable = FALSE; psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX; psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1; psoDesc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM; psoDesc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso));
    
    ComPtr<ID3D12DescriptorHeap> privateRtvHeap;
    D3D12_DESCRIPTOR_HEAP_DESC privateRtvHeapDesc = {};
    privateRtvHeapDesc.NumDescriptors = 1;
    privateRtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    g_device->CreateDescriptorHeap(&privateRtvHeapDesc, IID_PPV_ARGS(&privateRtvHeap));
    g_device->CreateRenderTargetView(privateTexture.Get(), nullptr, privateRtvHeap->GetCPUDescriptorHandleForHeapStart());

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    UINT64 textureFrameCount = 0;
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        FindAndConnectToProducer();

        if (g_isConnected && g_sharedFenceIn->GetCompletedValue() > g_lastSeenFrame) {
            g_lastSeenFrame = g_sharedFenceIn->GetCompletedValue();
            
            PopulateCommandList(pso.Get(), rootSignature.Get(), g_sharedBufferIn.Get(), privateTexture.Get(), sharedTexture.Get(), privateRtvHeap, privateRtvHeap->GetCPUDescriptorHandleForHeapStart(), TEX_WIDTH, TEX_HEIGHT);
            ID3D12CommandList* lists[] = { g_commandList.Get() };
            g_commandQueue->ExecuteCommandLists(1, lists);
            
            textureFrameCount++;
            g_commandQueue->Signal(sharedTextureFence.Get(), textureFrameCount);
            InterlockedExchange64(reinterpret_cast<volatile LONGLONG*>(&texManifestOutView->frameValue), textureFrameCount);
            WakeByAddressAll(&texManifestOutView->frameValue);
        }
        
        g_commandAllocator->Reset();
        g_commandList->Reset(g_commandAllocator.Get(), nullptr);
        
        if (g_isConnected) {
            D3D12_RESOURCE_BARRIER preCopyBarrier = {};
            preCopyBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            preCopyBarrier.Transition.pResource = g_renderTargets[g_frameIndex].Get();
            preCopyBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            preCopyBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            g_commandList->ResourceBarrier(1, &preCopyBarrier);
            g_commandList->CopyResource(g_renderTargets[g_frameIndex].Get(), privateTexture.Get());
            D3D12_RESOURCE_BARRIER postCopyBarrier = preCopyBarrier;
            postCopyBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            postCopyBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            g_commandList->ResourceBarrier(1, &postCopyBarrier);
        } else {
            // Render idle screen
            D3D12_RESOURCE_BARRIER presentToRT = {};
            presentToRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            presentToRT.Transition.pResource = g_renderTargets[g_frameIndex].Get();
            presentToRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            presentToRT.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            g_commandList->ResourceBarrier(1, &presentToRT);

            D3D12_CPU_DESCRIPTOR_HANDLE currentRtvHandle(g_rtvHeap->GetCPUDescriptorHandleForHeapStart());
            currentRtvHandle.ptr += g_frameIndex * g_rtvDescriptorSize;
            const float clearColor[] = { 0.1f, 0.0f, 0.1f, 1.0f };
            g_commandList->ClearRenderTargetView(currentRtvHandle, clearColor, 0, nullptr);
            
            D3D12_RESOURCE_BARRIER rtToPresent = presentToRT;
            rtToPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            rtToPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            g_commandList->ResourceBarrier(1, &rtToPresent);
        }
        
        g_commandList->Close();
        ID3D12CommandList* lists[] = { g_commandList.Get() };
        g_commandQueue->ExecuteCommandLists(1, lists);
        
        g_swapChain->Present(1, 0);
        WaitForPreviousFrame();
    }

    DisconnectFromProducer();
    return 0;
}

void FindAndConnectToProducer() {
    static auto lastSearchTime = std::chrono::steady_clock::now() - std::chrono::seconds(2);

    if (g_isConnected) {
        HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, g_producerPid);
        if (hProcess == NULL || WaitForSingleObject(hProcess, 0) != WAIT_TIMEOUT) {
            DisconnectFromProducer();
        }
        if (hProcess) CloseHandle(hProcess);
        return;
    }

    if (std::chrono::steady_clock::now() - lastSearchTime < std::chrono::seconds(2)) {
        return;
    }
    lastSearchTime = std::chrono::steady_clock::now();

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe32 = {};
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            std::wstring manifestName = L"DirectPort_BufferManifest_" + std::to_wstring(pe32.th32ProcessID);
            HANDLE hManifest = OpenFileMappingW(FILE_MAP_READ, FALSE, manifestName.c_str());
            if (hManifest) {
                SharedBufferManifest* manifestView = (SharedBufferManifest*)MapViewOfFile(hManifest, FILE_MAP_READ, 0, 0, sizeof(SharedBufferManifest));
                if (manifestView) {
                    HANDLE hFence, hBuffer;
                    g_device->OpenSharedHandleByName(manifestView->fenceName, GENERIC_ALL, &hFence);
                    g_device->OpenSharedHandleByName(manifestView->resourceName, GENERIC_ALL, &hBuffer);
                    
                    if (hFence && hBuffer && SUCCEEDED(g_device->OpenSharedHandle(hFence, IID_PPV_ARGS(&g_sharedFenceIn))) && SUCCEEDED(g_device->OpenSharedHandle(hBuffer, IID_PPV_ARGS(&g_sharedBufferIn)))) {
                        g_isConnected = true;
                        g_producerPid = pe32.th32ProcessID;
                        g_lastSeenFrame = 0;
                        UpdateWindowTitle();
                        UnmapViewOfFile(manifestView);
                        CloseHandle(hManifest);
                        CloseHandle(hFence);
                        CloseHandle(hBuffer);
                        CloseHandle(hSnapshot);
                        return;
                    }
                    if(hFence) CloseHandle(hFence);
                    if(hBuffer) CloseHandle(hBuffer);
                    UnmapViewOfFile(manifestView);
                }
                CloseHandle(hManifest);
            }
        } while (Process32NextW(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
}

void DisconnectFromProducer() {
    if (!g_isConnected) return;
    g_isConnected = false;
    g_producerPid = 0;
    g_sharedBufferIn.Reset();
    g_sharedFenceIn.Reset();
    UpdateWindowTitle();
}

void UpdateWindowTitle() {
    std::wstring title;
    if (g_isConnected) {
        title = L"Buffer To Texture D3D12 - Connected to PID " + std::to_wstring(g_producerPid);
    } else {
        title = L"Buffer To Texture D3D12 - Searching for producer...";
    }
    SetWindowTextW(g_hwnd, title.c_str());
}

// ... (The rest of the file remains the same) ...

void PopulateCommandList(ID3D12PipelineState* pso, ID3D12RootSignature* rootSig, ID3D12Resource* cb, ID3D12Resource* privateTex, ID3D12Resource* sharedTex, ComPtr<ID3D12DescriptorHeap> rtvHeap, const D3D12_CPU_DESCRIPTOR_HANDLE& rtvHandle, UINT texWidth, UINT texHeight) {
    g_commandAllocator->Reset();
    g_commandList->Reset(g_commandAllocator.Get(), pso);
    
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = privateTex;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_commandList->ResourceBarrier(1, &barrier);
    
    g_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    g_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    
    g_commandList->SetGraphicsRootSignature(rootSig);
    g_commandList->SetGraphicsRootConstantBufferView(0, cb->GetGPUVirtualAddress());
    
    g_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)texWidth, (float)texHeight, 0.0f, 1.0f };
    g_commandList->RSSetViewports(1, &viewport);
    D3D12_RECT scissorRect = { 0, 0, (LONG)texWidth, (LONG)texHeight };
    g_commandList->RSSetScissorRects(1, &scissorRect);
    g_commandList->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER barriers[2] = {};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = privateTex;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = sharedTex;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g_commandList->ResourceBarrier(2, barriers);

    g_commandList->CopyResource(sharedTex, privateTex);
    
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    g_commandList->ResourceBarrier(2, barriers);
    
    g_commandList->Close();
}

void WaitForPreviousFrame() {
    const UINT64 fence = g_fenceValue;
    g_commandQueue->Signal(g_fence.Get(), fence);
    g_fenceValue++;
    if (g_fence->GetCompletedValue() < fence) {
        g_fence->SetEventOnCompletion(fence, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}