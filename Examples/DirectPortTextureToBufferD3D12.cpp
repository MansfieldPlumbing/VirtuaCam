#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl.h>
#include <sddl.h>
#include <string> // <--- FIX: Added the missing header for std::wstring
#include <iostream>
#include <vector>
#include <tlhelp32.h>
#include <shellapi.h>
#include <chrono>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "Synchronization.lib")

using namespace Microsoft::WRL;

struct BroadcastManifest {
    UINT64 frameValue; UINT width; UINT height; DXGI_FORMAT format;
    LUID adapterLuid; WCHAR textureName[256]; WCHAR fenceName[256];
};
struct SharedBufferManifest {
    UINT64 frameValue; UINT64 bufferSize; LUID adapterLuid;
    WCHAR resourceName[256]; WCHAR fenceName[256];
};

const char* g_computeShaderHLSL = R"(
Texture2D<float4> g_inputTexture : register(t0);
RWStructuredBuffer<float> g_outputBuffer : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    float3 color = g_inputTexture[DTid.xy].rgb;
    float luminance = dot(color, float3(0.299, 0.587, 0.114));
    InterlockedAdd(g_outputBuffer[0], luminance);
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
void PopulateCommandList(ID3D12PipelineState* pso, ID3D12RootSignature* rootSig, ComPtr<ID3D12DescriptorHeap> heap, ID3D12Resource* privateTex, ID3D12Resource* sharedTex, ID3D12Resource* resultBuf, ID3D12Resource* sharedBuf, UINT width, UINT height);
void UpdateWindowTitle();
void DisconnectFromProducer();
void FindAndConnectToProducer();


// --- State Globals ---
static bool g_isConnected = false;
static DWORD g_producerPid = 0;
static ComPtr<ID3D12Resource> g_sharedTextureIn;
static ComPtr<ID3D12Resource> g_privateTexture;
static ComPtr<ID3D12Fence> g_sharedFenceIn;
static UINT64 g_lastSeenFrame = 0;
static BroadcastManifest g_inputManifest = {};

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int nCmdShow) {
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance, NULL, NULL, NULL, NULL, L"TextureToBufferD3D12", NULL };
    RegisterClassExW(&wc);
    g_hwnd = CreateWindowW(wc.lpszClassName, L"Texture To Buffer D3D12", WS_OVERLAPPEDWINDOW, 100, 100, 800, 600, NULL, NULL, hInstance, NULL);
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
    
    const UINT64 bufferSize = sizeof(float);
    ComPtr<ID3D12Resource> sharedBuffer;
    D3D12_HEAP_PROPERTIES defaultHeapProps = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufferDesc.Width = bufferSize; bufferDesc.Height = 1; bufferDesc.DepthOrArraySize = 1; bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1; bufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    g_device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_SHARED, &bufferDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&sharedBuffer));
    
    PSECURITY_DESCRIPTOR sd = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd, NULL);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd, FALSE };
    DWORD selfPid = GetCurrentProcessId();
    std::wstring resourceName = L"Global\\DirectPort_Buffer_" + std::to_wstring(selfPid);
    std::wstring fenceName = L"Global\\DirectPort_BufferFence_" + std::to_wstring(selfPid);
    std::wstring manifestName = L"DirectPort_BufferManifest_" + std::to_wstring(selfPid);

    HANDLE bufferHandle = nullptr;
    g_device->CreateSharedHandle(sharedBuffer.Get(), &sa, GENERIC_ALL, resourceName.c_str(), &bufferHandle);
    ComPtr<ID3D12Fence> sharedBufferFence;
    g_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&sharedBufferFence));
    HANDLE fenceHandle = nullptr;
    g_device->CreateSharedHandle(sharedBufferFence.Get(), &sa, GENERIC_ALL, fenceName.c_str(), &fenceHandle);
    HANDLE hBufManifest = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(SharedBufferManifest), manifestName.c_str());
    LocalFree(sd);
    SharedBufferManifest* bufManifestView = (SharedBufferManifest*)MapViewOfFile(hBufManifest, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    
    bufManifestView->frameValue = 0; bufManifestView->bufferSize = bufferSize; bufManifestView->adapterLuid = g_device->GetAdapterLuid();
    wcscpy_s(bufManifestView->resourceName, resourceName.c_str());
    wcscpy_s(bufManifestView->fenceName, fenceName.c_str());

    ComPtr<ID3DBlob> computeShader, errors;
    D3DCompile(g_computeShaderHLSL, strlen(g_computeShaderHLSL), nullptr, nullptr, nullptr, "main", "cs_5_1", 0, 0, &computeShader, &errors);
    
    D3D12_DESCRIPTOR_RANGE1 ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 1; ranges[0].BaseShaderRegister = 0;
    ranges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC; ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 1; ranges[1].BaseShaderRegister = 0;
    ranges[1].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE; ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER1 rootParameters[2] = {};
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[0].DescriptorTable.NumDescriptorRanges = 1; rootParameters[0].DescriptorTable.pDescriptorRanges = &ranges[0];
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[1].DescriptorTable.NumDescriptorRanges = 1; rootParameters[1].DescriptorTable.pDescriptorRanges = &ranges[1];
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rootSigDesc.Desc_1_1.NumParameters = _countof(rootParameters);
    rootSigDesc.Desc_1_1.pParameters = rootParameters;
    
    ComPtr<ID3DBlob> signature;
    D3D12SerializeVersionedRootSignature(&rootSigDesc, &signature, &errors);
    ComPtr<ID3D12RootSignature> rootSignature;
    g_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&rootSignature));
    
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = rootSignature.Get();
    psoDesc.CS = { computeShader->GetBufferPointer(), computeShader->GetBufferSize() };
    ComPtr<ID3D12PipelineState> pso;
    g_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso));
    
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = 2; heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap));
    
    ComPtr<ID3D12Resource> resultBuffer;
    bufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    g_device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resultBuffer));
    
    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    UINT64 bufferFrameCount = 0;
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
            
            PopulateCommandList(pso.Get(), rootSignature.Get(), descriptorHeap, g_privateTexture.Get(), g_sharedTextureIn.Get(), resultBuffer.Get(), sharedBuffer.Get(), g_inputManifest.width, g_inputManifest.height);
            ID3D12CommandList* lists[] = { g_commandList.Get() };
            g_commandQueue->ExecuteCommandLists(1, lists);
            
            bufferFrameCount++;
            g_commandQueue->Signal(sharedBufferFence.Get(), bufferFrameCount);
            InterlockedExchange64(reinterpret_cast<volatile LONGLONG*>(&bufManifestView->frameValue), bufferFrameCount);
            WakeByAddressAll(&bufManifestView->frameValue);
        }

        g_commandAllocator->Reset();
        g_commandList->Reset(g_commandAllocator.Get(), nullptr);
        
        D3D12_RESOURCE_BARRIER preCopyBarrier = {};
        preCopyBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        preCopyBarrier.Transition.pResource = g_renderTargets[g_frameIndex].Get();
        preCopyBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        preCopyBarrier.Transition.StateAfter = g_isConnected ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_commandList->ResourceBarrier(1, &preCopyBarrier);
        
        if (g_isConnected) {
            g_commandList->CopyResource(g_renderTargets[g_frameIndex].Get(), g_privateTexture.Get());
        } else {
            D3D12_CPU_DESCRIPTOR_HANDLE currentRtvHandle(g_rtvHeap->GetCPUDescriptorHandleForHeapStart());
            currentRtvHandle.ptr += g_frameIndex * g_rtvDescriptorSize;
            const float clearColor[] = { 0.1f, 0.0f, 0.1f, 1.0f }; // Dark purple for idle
            g_commandList->ClearRenderTargetView(currentRtvHandle, clearColor, 0, nullptr);
        }
        
        D3D12_RESOURCE_BARRIER postCopyBarrier = preCopyBarrier;
        postCopyBarrier.Transition.StateBefore = g_isConnected ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_RENDER_TARGET;
        postCopyBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_commandList->ResourceBarrier(1, &postCopyBarrier);
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
    const std::vector<std::wstring> prefixes = { L"DirectPort_Producer_Manifest_", L"D3D12_Producer_Manifest_" };

    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            HANDLE hManifest = nullptr;
            for(const auto& prefix : prefixes) {
                std::wstring manifestName = prefix + std::to_wstring(pe32.th32ProcessID);
                hManifest = OpenFileMappingW(FILE_MAP_READ, FALSE, manifestName.c_str());
                if(hManifest) break;
            }

            if (hManifest) {
                BroadcastManifest* manifestView = (BroadcastManifest*)MapViewOfFile(hManifest, FILE_MAP_READ, 0, 0, sizeof(BroadcastManifest));
                if (manifestView) {
                    HANDLE hFence = nullptr, hTexture = nullptr;
                    g_device->OpenSharedHandleByName(manifestView->fenceName, GENERIC_ALL, &hFence);
                    g_device->OpenSharedHandleByName(manifestView->textureName, GENERIC_ALL, &hTexture);
                    
                    if (hFence && hTexture && SUCCEEDED(g_device->OpenSharedHandle(hFence, IID_PPV_ARGS(&g_sharedFenceIn))) && SUCCEEDED(g_device->OpenSharedHandle(hTexture, IID_PPV_ARGS(&g_sharedTextureIn)))) {
                        g_isConnected = true;
                        g_producerPid = pe32.th32ProcessID;
                        g_lastSeenFrame = 0;
                        memcpy(&g_inputManifest, manifestView, sizeof(BroadcastManifest));
                        
                        D3D12_RESOURCE_DESC privateTexDesc = g_sharedTextureIn->GetDesc();
                        privateTexDesc.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                        D3D12_HEAP_PROPERTIES defaultHeapProps = { D3D12_HEAP_TYPE_DEFAULT };
                        g_device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_NONE, &privateTexDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_privateTexture));
                        
                        UpdateWindowTitle();
                        UnmapViewOfFile(manifestView);
                        CloseHandle(hManifest);
                        CloseHandle(hFence);
                        CloseHandle(hTexture);
                        CloseHandle(hSnapshot);
                        return;
                    }
                    if(hFence) CloseHandle(hFence);
                    if(hTexture) CloseHandle(hTexture);
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
    g_sharedTextureIn.Reset();
    g_privateTexture.Reset();
    g_sharedFenceIn.Reset();
    UpdateWindowTitle();
}

void UpdateWindowTitle() {
    std::wstring title;
    if (g_isConnected) {
        title = L"Texture To Buffer D3D12 - Connected to PID " + std::to_wstring(g_producerPid);
    } else {
        title = L"Texture To Buffer D3D12 - Searching for producer...";
    }
    SetWindowTextW(g_hwnd, title.c_str());
}

void PopulateCommandList(ID3D12PipelineState* pso, ID3D12RootSignature* rootSig, ComPtr<ID3D12DescriptorHeap> heap, ID3D12Resource* privateTex, ID3D12Resource* sharedTex, ID3D12Resource* resultBuf, ID3D12Resource* sharedBuf, UINT width, UINT height)
{
    g_commandAllocator->Reset();
    g_commandList->Reset(g_commandAllocator.Get(), pso);
    
    g_commandList->CopyResource(privateTex, sharedTex);
    
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resultBuf;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    g_commandList->ResourceBarrier(1, &barrier);
    
    D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = heap->GetCPUDescriptorHandleForHeapStart();
    uavHandle.ptr += g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const float clearValues[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    
    // Create the SRV and UAV on the fly inside the descriptor heap
    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = heap->GetCPUDescriptorHandleForHeapStart();
    g_device->CreateShaderResourceView(privateTex, nullptr, srvHandle);
    g_device->CreateUnorderedAccessView(resultBuf, nullptr, nullptr, uavHandle);

    g_commandList->ClearUnorderedAccessViewFloat(heap->GetGPUDescriptorHandleForHeapStart(), uavHandle, resultBuf, clearValues, 0, nullptr);

    g_commandList->SetComputeRootSignature(rootSig);
    ID3D12DescriptorHeap* heaps[] = { heap.Get() };
    g_commandList->SetDescriptorHeaps(1, heaps);
    g_commandList->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    g_commandList->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
    
    g_commandList->Dispatch(width / 8, height / 8, 1);
    
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_commandList->ResourceBarrier(1, &barrier);
    
    g_commandList->CopyResource(sharedBuf, resultBuf);
    
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