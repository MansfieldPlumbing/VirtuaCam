#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <sddl.h>
#include <string>
#include <atomic>
#include <vector>
#include <d3dcompiler.h>
#include <shlwapi.h>
#include <chrono>
#include "resource.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "Synchronization.lib")
#pragma comment(lib, "dxguid.lib")

#if defined(__cplusplus)
extern "C" {
    struct __declspec(uuid("189819f1-1db6-4b57-be54-1821339b85f7")) IID_ID3D12Device;
}
#endif

using Microsoft::WRL::ComPtr;

void Log(const std::wstring& msg) { WCHAR b[512]; wsprintfW(b, L"[DirectPort Camera] %s\n", msg.c_str()); OutputDebugStringW(b); }
void LogHRESULT(const std::wstring& msg, HRESULT hr) { WCHAR b[512]; wsprintfW(b, L"[DirectPort Camera] %s - HRESULT: 0x%08X\n", msg.c_str(), hr); OutputDebugStringW(b); }

struct BroadcastManifest {
    UINT64 frameValue;
    UINT width;
    UINT height;
    DXGI_FORMAT format;
    LUID adapterLuid;
    WCHAR textureName[256];
    WCHAR fenceName[256];
};

struct ShaderConstants {
    UINT videoWidth;
    UINT videoHeight;
};

static const UINT kFrameCount = 2;
HWND g_hWnd = nullptr;
ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12CommandQueue> g_commandQueue;
ComPtr<IDXGISwapChain3> g_swapChain;
ComPtr<ID3D12Resource> g_renderTargets[kFrameCount];
ComPtr<ID3D12CommandAllocator> g_commandAllocators[kFrameCount];
ComPtr<ID3D12GraphicsCommandList> g_commandList;
ComPtr<ID3D12RootSignature> g_rootSignature;
ComPtr<ID3D12PipelineState> g_psoYUY2;
ComPtr<ID3D12PipelineState> g_psoNV12;
ComPtr<ID3D12DescriptorHeap> g_rtvHeap;
ComPtr<ID3D12DescriptorHeap> g_srvHeap;
UINT g_rtvDescriptorSize;
UINT g_srvDescriptorSize;
UINT g_frameIndex;
ComPtr<ID3D12Fence> g_renderFence;
UINT64 g_frameFenceValues[kFrameCount];
HANDLE g_fenceEvent;
UINT64 g_fenceValue = 1;
ComPtr<ID3D12Resource> g_vertexBuffer;
ComPtr<ID3D12Resource> g_vertexBufferMirrored;
D3D12_VERTEX_BUFFER_VIEW g_vertexBufferView;
D3D12_VERTEX_BUFFER_VIEW g_vertexBufferViewMirrored;
ComPtr<ID3D12Resource> g_constantBuffer;
UINT8* g_pCbvDataBegin = nullptr;

ComPtr<ID3D12Resource> g_pCameraTextureY;
ComPtr<ID3D12Resource> g_pCameraTextureUV;
ComPtr<ID3D12Resource> g_uploadHeap;
const UINT64 UPLOAD_HEAP_SIZE = 4096 * 2160 * 4;

std::vector<BYTE> g_cpuFrameBuffers[3];
std::atomic<int> g_q_write_idx = 0;
std::atomic<int> g_q_read_idx = 0;
std::atomic<bool> g_bIsCameraStreamReady = false;

static ComPtr<ID3D12Resource>       g_sharedTexture;
static ComPtr<ID3D12DescriptorHeap>  g_sharedRtvHeap;
static ComPtr<ID3D12Fence>          g_sharedFence;
static UINT64                       g_sharedFrameValue = 0;
static std::wstring                 g_sharedTextureName;
static std::wstring                 g_sharedFenceName;
static HANDLE                       g_sharedTextureHandle = nullptr;
static HANDLE                       g_sharedFenceHandle = nullptr;

static std::vector<UINT>            g_sharedResolutions = { 128, 240, 360, 480, 720, 1080, 1440, 2160 };
static int                          g_currentResolutionIndex = 3;
static UINT                         g_currentSharedSize = g_sharedResolutions[3];
static HANDLE                       g_hManifest = nullptr;
static BroadcastManifest*           g_pManifestView = nullptr;

int g_currentCameraIndex = -1;
ComPtr<IMFSourceReader> g_pSourceReader;
long g_videoWidth = 0;
long g_videoHeight = 0;
GUID g_videoFormat = { 0 };
CRITICAL_SECTION g_critSec;
bool g_bFullscreen = false;
std::atomic<bool> g_bMirror = false;
#define IDT_SINGLECLICK_TIMER 1

HRESULT InitWindow(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
HRESULT InitD3D12();
HRESULT LoadAssets();
void PopulateCommandList();
void WaitForGpuIdle();
void MoveToNextFrame();
void Cleanup();
HRESULT InternalCycleCamera();
void ToggleFullscreen();
void OnResize(UINT width, UINT height);
void ShutdownSharing();
HRESULT InitializeSharing(UINT width, UINT height);
void InternalChangeSharedResolution(BOOL bCycleUp);
std::wstring GetResolutionType(UINT width, UINT height);

extern "C" {
    __declspec(dllexport) void CycleCameraSource();
    __declspec(dllexport) void CycleSharedResolution(BOOL bCycleUp);
    __declspec(dllexport) void ToggleMirrorStream();
}
void CycleCameraSource() { PostMessage(g_hWnd, WM_APP + 1, 0, 0); }
void CycleSharedResolution(BOOL bCycleUp) { PostMessage(g_hWnd, WM_APP + 2, bCycleUp, 0); }
void ToggleMirrorStream() { PostMessage(g_hWnd, WM_APP + 3, 0, 0); }

const char* g_shaderHLSL = R"(
cbuffer Constants : register(b0) { uint2 videoDimensions; };
struct VSInput
{
    float3 position : POSITION;
    float2 uv : TEXCOORD;
};
struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};
PSInput VSMain(VSInput input)
{
    PSInput output;
    output.position = float4(input.position, 1.0f);
    output.uv = input.uv;
    return output;
}

Texture2D<float4> g_textureYUY2 : register(t0);
Texture2D<float>  g_textureY    : register(t0);
Texture2D<float2> g_textureUV   : register(t1);
SamplerState      g_sampler     : register(s0);

float3 YUVtoRGB_BT709(float y, float u, float v) {
    y = (y - (16.0/255.0)) * (255.0/219.0);
    u = u - 0.5;
    v = v - 0.5;
    float r = y + 1.5748 * v;
    float g = y - 0.1873 * u - 0.4681 * v;
    float b = y + 1.8556 * u;
    return saturate(float3(r, g, b));
}
float4 PS_YUY2(PSInput input) : SV_TARGET {
    float4 yuyv = g_textureYUY2.Sample(g_sampler, input.uv);
    float y;
    float u = yuyv.y;
    float v = yuyv.w;
    if (frac(input.uv.x * (videoDimensions.x / 2.0f)) > 0.5f) {
        y = yuyv.z;
    } else {
        y = yuyv.x;
    }
    float3 rgb = YUVtoRGB_BT709(y, u, v);
    return float4(rgb, 1.0f);
}
float4 PS_NV12(PSInput input) : SV_TARGET {
    float y = g_textureY.Sample(g_sampler, input.uv).r;
    float2 uv = g_textureUV.Sample(g_sampler, input.uv).rg;
    float3 rgb = YUVtoRGB_BT709(y, uv.x, uv.y);
    return float4(rgb, 1.0f);
}
)";

class CaptureManager : public IMFSourceReaderCallback
{
public:
    static HRESULT CreateInstance(CaptureManager** ppManager)
    {
        if (!ppManager) return E_POINTER;
        *ppManager = new (std::nothrow) CaptureManager();
        return (*ppManager) ? S_OK : E_OUTOFMEMORY;
    }

    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) override
    {
        static const QITAB qit[] = {
            QITABENT(CaptureManager, IMFSourceReaderCallback),
            {0}
        };
        return QISearch(this, qit, iid, ppv);
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_refCount); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG u = InterlockedDecrement(&m_refCount);
        if (!u) delete this;
        return u;
    }

    STDMETHODIMP OnReadSample(HRESULT hrStatus, DWORD, DWORD, LONGLONG, IMFSample* pSample) override
    {
        if (SUCCEEDED(hrStatus) && pSample)
        {
            Microsoft::WRL::ComPtr<IMFMediaBuffer> pBuffer;
            if (SUCCEEDED(pSample->ConvertToContiguousBuffer(&pBuffer)))
            {
                const UINT W = UINT(g_videoWidth);
                const UINT H = UINT(g_videoHeight);

                int writeSlot = g_q_write_idx.load(std::memory_order_relaxed);
                int nextWrite = (writeSlot + 1) % 3;

                if (nextWrite != g_q_read_idx.load(std::memory_order_acquire))
                {
                    size_t expectedSize = 0;
                    if (g_videoFormat == MFVideoFormat_YUY2) {
                        expectedSize = size_t(W) * H * 2;
                    } else {
                        expectedSize = size_t(W) * H + (size_t(W) * H / 2);
                    }

                    if (g_cpuFrameBuffers[writeSlot].size() == expectedSize)
                    {
                        BYTE* dst = g_cpuFrameBuffers[writeSlot].data();
                        
                        Microsoft::WRL::ComPtr<IMF2DBuffer2> p2DBuffer2;
                        if (SUCCEEDED(pBuffer.As(&p2DBuffer2)))
                        {
                            BYTE* pScanline0 = nullptr;
                            LONG lPitch = 0;
                            BYTE* pBufferStart = nullptr;
                            DWORD cbBufferLength = 0;

                            if (SUCCEEDED(p2DBuffer2->Lock2DSize(MF2DBuffer_LockFlags_Read, &pScanline0, &lPitch, &pBufferStart, &cbBufferLength)))
                            {
                                if (g_videoFormat == MFVideoFormat_YUY2)
                                {
                                    const UINT rowBytes = W * 2;
                                    for (UINT y = 0; y < H; ++y) {
                                        memcpy(dst + y * rowBytes, pScanline0 + y * lPitch, rowBytes);
                                    }
                                }
                                else
                                {
                                    BYTE* y_dst = dst;
                                    BYTE* y_src = pScanline0;
                                    for (UINT y = 0; y < H; ++y) {
                                        memcpy(y_dst, y_src, W);
                                        y_dst += W;
                                        y_src += lPitch;
                                    }

                                    BYTE* uv_dst = dst + (size_t(W) * H);
                                    BYTE* uv_src = pScanline0 + (size_t(lPitch) * H);
                                    const UINT uv_height = H / 2;
                                    for (UINT y = 0; y < uv_height; ++y) {
                                        memcpy(uv_dst, uv_src, W);
                                        uv_dst += W;
                                        uv_src += lPitch;
                                    }
                                }
                                
                                g_q_write_idx.store(nextWrite, std::memory_order_release);
                                g_bIsCameraStreamReady.store(true, std::memory_order_release);

                                p2DBuffer2->Unlock2D();
                            }
                        }
                    }
                }
            }
        }

        EnterCriticalSection(&g_critSec);
        if (g_pSourceReader) {
            g_pSourceReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);
        }
        LeaveCriticalSection(&g_critSec);
        return S_OK;
    }

    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }
    STDMETHODIMP OnFlush(DWORD) override { return S_OK; }

private:
    CaptureManager() : m_refCount(1) {}
    ~CaptureManager() {}

    long m_refCount;
};


int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    DWORD pid = GetCurrentProcessId();
    g_sharedTextureName = L"Global\\DirectPortTexture_" + std::to_wstring(pid);
    g_sharedFenceName = L"Global\\DirectPortFence_" + std::to_wstring(pid);
    if (FAILED(InitWindow(hInstance, nCmdShow))) return 0;
    InitializeCriticalSection(&g_critSec);
    MFStartup(MF_VERSION);
    if (SUCCEEDED(InitD3D12()) && SUCCEEDED(LoadAssets())) {
        InternalCycleCamera();
        MSG msg = { 0 };
        while (WM_QUIT != msg.message) {
            if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            else {
                PopulateCommandList();
                ID3D12CommandList* ppCommandLists[] = { g_commandList.Get() };
                g_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
                if (g_sharedFence) {
                    g_commandQueue->Signal(g_sharedFence.Get(), ++g_sharedFrameValue);
                    if (g_pManifestView) {
                         g_pManifestView->frameValue = g_sharedFrameValue;
                         WakeByAddressAll(&g_pManifestView->frameValue);
                    }
                }

                g_commandQueue->Signal(g_renderFence.Get(), g_fenceValue);
                g_frameFenceValues[g_frameIndex] = g_fenceValue;
                g_fenceValue++;
                
                g_swapChain->Present(1, 0);
                MoveToNextFrame();
            }
        }
    }
    Cleanup();
    MFShutdown();
    DeleteCriticalSection(&g_critSec);
    return 0;
}

HRESULT InitWindow(HINSTANCE hInstance, int nCmdShow) {
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEXW) };
    wcex.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.lpszClassName = L"DirectPortCameraApp";
    wcex.hIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(DPD_ICON1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    wcex.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(DPD_ICON1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);

    if (!RegisterClassExW(&wcex)) return E_FAIL;

    WCHAR title[256];
    wsprintfW(title, L"DirectPort Camera (PID: %lu)", GetCurrentProcessId());
    g_hWnd = CreateWindowW(L"DirectPortCameraApp", title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, nullptr, nullptr, hInstance, nullptr);
    if (!g_hWnd) return E_FAIL;

    SendMessage(g_hWnd, WM_SETICON, ICON_BIG, (LPARAM)wcex.hIcon);
    SendMessage(g_hWnd, WM_SETICON, ICON_SMALL, (LPARAM)wcex.hIconSm);

    ShowWindow(g_hWnd, nCmdShow);
    return S_OK;
}

void InternalChangeSharedResolution(BOOL bCycleUp) {
    if (bCycleUp) g_currentResolutionIndex = (g_currentResolutionIndex + 1) % g_sharedResolutions.size();
    else g_currentResolutionIndex = (g_currentResolutionIndex == 0) ? (g_sharedResolutions.size() - 1) : (g_currentResolutionIndex - 1);
    UINT newSize = g_sharedResolutions[g_currentResolutionIndex];
    if (g_currentSharedSize == newSize) return;

    Log(L"Changing shared resolution to " + std::to_wstring(newSize) + L"x" + std::to_wstring(newSize));
    std::wstring resType = GetResolutionType(newSize, newSize);
    WCHAR title[256];
    wsprintfW(title, L"DirectPort Camera (PID: %lu) (Shared: %up %s)", GetCurrentProcessId(), newSize, resType.c_str());
    SetWindowTextW(g_hWnd, title);
    EnterCriticalSection(&g_critSec);
    WaitForGpuIdle();
    ShutdownSharing();
    g_currentSharedSize = newSize;
    if (FAILED(InitializeSharing(g_currentSharedSize, g_currentSharedSize))) {
        Log(L"Failed to re-initialize sharing session.");
    }
    LeaveCriticalSection(&g_critSec);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_DESTROY: PostQuitMessage(0); break;
    case WM_LBUTTONDOWN: SetTimer(hWnd, IDT_SINGLECLICK_TIMER, GetDoubleClickTime(), NULL); break;
    case WM_LBUTTONDBLCLK: KillTimer(hWnd, IDT_SINGLECLICK_TIMER); ToggleFullscreen(); break;
    case WM_TIMER: if (wParam == IDT_SINGLECLICK_TIMER) { KillTimer(hWnd, IDT_SINGLECLICK_TIMER); InternalCycleCamera(); } break;
    case WM_KEYDOWN: if (wParam == VK_RETURN) { ToggleFullscreen(); } break;
    case WM_MOUSEWHEEL: InternalChangeSharedResolution(GET_WHEEL_DELTA_WPARAM(wParam) > 0); break;
    case WM_RBUTTONDOWN: g_bMirror = !g_bMirror; break;
    case WM_APP + 1: InternalCycleCamera(); break;
    case WM_APP + 2: InternalChangeSharedResolution((BOOL)wParam); break;
    case WM_APP + 3: g_bMirror = !g_bMirror; break;
    case WM_SIZE: if (g_swapChain && wParam != SIZE_MINIMIZED) { OnResize(LOWORD(lParam), HIWORD(lParam)); } break;
    default: return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

HRESULT InitD3D12() {
    UINT dxgiFactoryFlags = 0;
#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)))) return E_FAIL;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device)))) return E_FAIL;

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_commandQueue));

    RECT rc; GetClientRect(g_hWnd, &rc);
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = kFrameCount;
    swapChainDesc.Width = rc.right - rc.left;
    swapChainDesc.Height = rc.bottom - rc.top;
    swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChain;
    factory->CreateSwapChainForHwnd(g_commandQueue.Get(), g_hWnd, &swapChainDesc, nullptr, nullptr, &swapChain);
    swapChain.As(&g_swapChain);
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = kFrameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    g_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_rtvHeap));
    g_rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 2;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&g_srvHeap));
    g_srvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

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
    for (UINT n = 0; n < kFrameCount; n++) g_frameFenceValues[n] = 0;
    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (g_fenceEvent == nullptr) { return HRESULT_FROM_WIN32(GetLastError()); }
    
    D3D12_HEAP_PROPERTIES uploadHeapProps = { D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = UPLOAD_HEAP_SIZE;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    g_device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_uploadHeap));

    if (FAILED(InitializeSharing(g_currentSharedSize, g_currentSharedSize))) {
        Log(L"Failed to initialize sharing session. Will run as local viewer only.");
    }
    Log(L"InitD3D12 FINISHED successfully.");
    return S_OK;
}

HRESULT LoadAssets() {
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };

    D3D12_ROOT_PARAMETER rootParameters[2] = {};
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[0].Descriptor = { 0, 0 };
    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[1].DescriptorTable = { _countof(ranges), ranges };

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.ShaderRegister = 0;

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
    rootSignatureDesc.NumParameters = _countof(rootParameters);
    rootSignatureDesc.pParameters = rootParameters;
    rootSignatureDesc.NumStaticSamplers = 1;
    rootSignatureDesc.pStaticSamplers = &sampler;
    rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature, error;
    D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    g_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&g_rootSignature));

    ComPtr<ID3DBlob> vsBlob, psBlobYUY2, psBlobNV12;
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    compileFlags |= D3DCOMPILE_DEBUG;
#endif
    D3DCompile(g_shaderHLSL, strlen(g_shaderHLSL), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, &vsBlob, &error);
    D3DCompile(g_shaderHLSL, strlen(g_shaderHLSL), nullptr, nullptr, nullptr, "PS_YUY2", "ps_5_0", compileFlags, 0, &psBlobYUY2, &error);
    D3DCompile(g_shaderHLSL, strlen(g_shaderHLSL), nullptr, nullptr, nullptr, "PS_NV12", "ps_5_0", compileFlags, 0, &psBlobNV12, &error);

    D3D12_INPUT_ELEMENT_DESC inputElementDescs[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
    psoDesc.pRootSignature = g_rootSignature.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.RasterizerState = { D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE, FALSE, D3D12_DEFAULT_DEPTH_BIAS, D3D12_DEFAULT_DEPTH_BIAS_CLAMP, D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS, TRUE, FALSE, FALSE, 0, D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF };
    psoDesc.BlendState = { FALSE, FALSE, {} };
    psoDesc.BlendState.RenderTarget[0] = { FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL };
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    psoDesc.PS = { psBlobYUY2->GetBufferPointer(), psBlobYUY2->GetBufferSize() };
    g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_psoYUY2));

    psoDesc.PS = { psBlobNV12->GetBufferPointer(), psBlobNV12->GetBufferSize() };
    g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_psoNV12));
    
    struct SimpleVertex { float Pos[3]; float Tex[2]; };
    SimpleVertex vertices[] = { {{ -1, 1, .5f }, { 0,0 }},{{ 1, 1, .5f }, { 1,0 }},{{ -1,-1,.5f }, { 0,1 }},{{ 1,-1,.5f }, { 1,1 }} };
    const UINT vertexBufferSize = sizeof(vertices);

    D3D12_HEAP_PROPERTIES uploadHeapProps = { D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = vertexBufferSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    g_device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_vertexBuffer));
    g_device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_vertexBufferMirrored));
    
    UINT8* pVertexDataBegin;
    D3D12_RANGE readRange = {}; 
    g_vertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pVertexDataBegin));
    memcpy(pVertexDataBegin, vertices, sizeof(vertices));
    g_vertexBuffer->Unmap(0, nullptr);
    g_vertexBufferView = { g_vertexBuffer->GetGPUVirtualAddress(), vertexBufferSize, sizeof(SimpleVertex) };
    
    SimpleVertex verticesMirrored[] = { {{ -1,1,.5f },{ 1,0 }},{{ 1,1,.5f },{ 0,0 }},{{ -1,-1,.5f },{ 1,1 }},{{ 1,-1,.5f },{ 0,1 }} };
    g_vertexBufferMirrored->Map(0, &readRange, reinterpret_cast<void**>(&pVertexDataBegin));
    memcpy(pVertexDataBegin, verticesMirrored, sizeof(verticesMirrored));
    g_vertexBufferMirrored->Unmap(0, nullptr);
    g_vertexBufferViewMirrored = { g_vertexBufferMirrored->GetGPUVirtualAddress(), vertexBufferSize, sizeof(SimpleVertex) };

    bufferDesc.Width = 256;
    g_device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_constantBuffer));
    g_constantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&g_pCbvDataBegin));
    return S_OK;
}

void PopulateCommandList() {
    g_commandAllocators[g_frameIndex]->Reset();
    g_commandList->Reset(g_commandAllocators[g_frameIndex].Get(), nullptr);

    g_commandList->SetGraphicsRootSignature(g_rootSignature.Get());
    ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
    g_commandList->SetDescriptorHeaps(_countof(heaps), heaps);
    g_commandList->SetGraphicsRootConstantBufferView(0, g_constantBuffer->GetGPUVirtualAddress());

    const int read_slot = g_q_read_idx.load(std::memory_order_relaxed);
    if (g_bIsCameraStreamReady && read_slot != g_q_write_idx.load(std::memory_order_acquire)) {

        const BYTE* frame = g_cpuFrameBuffers[read_slot].data();
        const UINT W = static_cast<UINT>(g_videoWidth);
        const UINT H = static_cast<UINT>(g_videoHeight);

        bool isNV12 = g_videoFormat == MFVideoFormat_NV12 && g_pCameraTextureY && g_pCameraTextureUV;

        if (isNV12) {
            D3D12_RESOURCE_BARRIER preBarriers[2] = {};
            preBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            preBarriers[0].Transition.pResource = g_pCameraTextureY.Get();
            preBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            preBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            preBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            preBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            preBarriers[1].Transition.pResource = g_pCameraTextureUV.Get();
            preBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            preBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            preBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            g_commandList->ResourceBarrier(_countof(preBarriers), preBarriers);
        } else if (g_pCameraTextureY) {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = g_pCameraTextureY.Get();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            g_commandList->ResourceBarrier(1, &barrier);
        }

        if (isNV12) {
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT yLayout = {};
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT uvLayout = {};
            D3D12_RESOURCE_DESC yDesc = g_pCameraTextureY->GetDesc();
            D3D12_RESOURCE_DESC uvDesc = g_pCameraTextureUV->GetDesc();
            
            UINT64 totalSize = 0;
            g_device->GetCopyableFootprints(&yDesc, 0, 1, 0, &yLayout, nullptr, nullptr, &totalSize);
            g_device->GetCopyableFootprints(&uvDesc, 0, 1, totalSize, &uvLayout, nullptr, nullptr, nullptr);

            UINT8* uploadPtr = nullptr;
            g_uploadHeap->Map(0, nullptr, reinterpret_cast<void**>(&uploadPtr));

            const BYTE* pSrcY = frame;
            for (UINT y = 0; y < H; ++y) {
                memcpy(uploadPtr + yLayout.Offset + y * yLayout.Footprint.RowPitch, pSrcY + y * W, W);
            }
            
            const BYTE* pSrcUV = frame + (size_t)W * H;
            for (UINT y = 0; y < H / 2; ++y) {
                memcpy(uploadPtr + uvLayout.Offset + y * uvLayout.Footprint.RowPitch, pSrcUV + y * W, W);
            }

            g_uploadHeap->Unmap(0, nullptr);
            
            D3D12_TEXTURE_COPY_LOCATION DstY = {};
            DstY.pResource = g_pCameraTextureY.Get();
            DstY.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION SrcY = {};
            SrcY.pResource = g_uploadHeap.Get();
            SrcY.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            SrcY.PlacedFootprint = yLayout;
            g_commandList->CopyTextureRegion(&DstY, 0, 0, 0, &SrcY, nullptr);

            D3D12_TEXTURE_COPY_LOCATION DstUV = {};
            DstUV.pResource = g_pCameraTextureUV.Get();
            DstUV.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION SrcUV = {};
            SrcUV.pResource = g_uploadHeap.Get();
            SrcUV.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            SrcUV.PlacedFootprint = uvLayout;
            g_commandList->CopyTextureRegion(&DstUV, 0, 0, 0, &SrcUV, nullptr);

        } else if (g_pCameraTextureY) {
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT yuy2Layout = {};
            D3D12_RESOURCE_DESC yuy2Desc = g_pCameraTextureY->GetDesc();
            g_device->GetCopyableFootprints(&yuy2Desc, 0, 1, 0, &yuy2Layout, nullptr, nullptr, nullptr);
            
            UINT8* uploadPtr = nullptr;
            g_uploadHeap->Map(0, nullptr, reinterpret_cast<void**>(&uploadPtr));
            
            const BYTE* pSrcYUY2 = frame;
            const UINT srcRowPitch = W * 2;
            const UINT dstRowPitch = yuy2Layout.Footprint.RowPitch;
            for (UINT y = 0; y < H; ++y) {
                memcpy(uploadPtr + yuy2Layout.Offset + y * dstRowPitch, pSrcYUY2 + y * srcRowPitch, srcRowPitch);
            }
            g_uploadHeap->Unmap(0, nullptr);

            D3D12_TEXTURE_COPY_LOCATION Dst = { g_pCameraTextureY.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, 0 };
            D3D12_TEXTURE_COPY_LOCATION Src = { g_uploadHeap.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, yuy2Layout };
            g_commandList->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
        }

        if (isNV12) {
            D3D12_RESOURCE_BARRIER postBarriers[2] = {};
            postBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            postBarriers[0].Transition.pResource = g_pCameraTextureY.Get();
            postBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            postBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            postBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            postBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            postBarriers[1].Transition.pResource = g_pCameraTextureUV.Get();
            postBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            postBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            postBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            g_commandList->ResourceBarrier(_countof(postBarriers), postBarriers);
        } else if (g_pCameraTextureY) {
             D3D12_RESOURCE_BARRIER barrier = {};
             barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
             barrier.Transition.pResource = g_pCameraTextureY.Get();
             barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
             barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
             barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
             g_commandList->ResourceBarrier(1, &barrier);
        }
        
        g_q_read_idx.store((read_slot + 1) % 3, std::memory_order_release);
    }
    
    auto RenderCameraScene = [&](D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle, UINT width, UINT height, bool isMainWindow) {
        g_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
        D3D12_VIEWPORT vp{ 0.f, 0.f, (FLOAT)width, (FLOAT)height, 0.f, 1.f };
        D3D12_RECT sr{ 0, 0, (LONG)width, (LONG)height };
        g_commandList->RSSetViewports(1, &vp);
        g_commandList->RSSetScissorRects(1, &sr);
        if (isMainWindow) {
            const float clearColor[] = { 0.0f, 0.0f, 0.2f, 1.0f };
            g_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        }
        if (g_bIsCameraStreamReady) {
            g_commandList->SetPipelineState((g_videoFormat == MFVideoFormat_YUY2) ? g_psoYUY2.Get() : g_psoNV12.Get());
            g_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            g_commandList->IASetVertexBuffers(0, 1, g_bMirror ? &g_vertexBufferViewMirrored : &g_vertexBufferView);
            g_commandList->SetGraphicsRootDescriptorTable(1, g_srvHeap->GetGPUDescriptorHandleForHeapStart());
            g_commandList->DrawInstanced(4, 1, 0, 0);
        } else if (!isMainWindow) {
            const float clearColor[] = { 0.0f, 0.0f, 0.1f, 1.0f };
            g_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        }
    };

    if (g_sharedTexture) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = g_sharedTexture.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_commandList->ResourceBarrier(1, &barrier);
        RenderCameraScene(g_sharedRtvHeap->GetCPUDescriptorHandleForHeapStart(), g_currentSharedSize, g_currentSharedSize, false);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        g_commandList->ResourceBarrier(1, &barrier);
    }

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = g_renderTargets[g_frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_commandList->ResourceBarrier(1, &barrier);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += g_frameIndex * g_rtvDescriptorSize;
    RECT rc; GetClientRect(g_hWnd, &rc);
    RenderCameraScene(rtvHandle, rc.right - rc.left, rc.bottom - rc.top, true);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_commandList->ResourceBarrier(1, &barrier);

    g_commandList->Close();
}

void WaitForGpuIdle() {
    g_commandQueue->Signal(g_renderFence.Get(), g_fenceValue);
    g_renderFence->SetEventOnCompletion(g_fenceValue, g_fenceEvent);
    WaitForSingleObject(g_fenceEvent, INFINITE);
    g_fenceValue++;
}

void MoveToNextFrame() {
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
    if (g_renderFence->GetCompletedValue() < g_frameFenceValues[g_frameIndex]) {
        g_renderFence->SetEventOnCompletion(g_frameFenceValues[g_frameIndex], g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }
}

void Cleanup() {
    WaitForGpuIdle();
    ShutdownSharing();
    CloseHandle(g_fenceEvent);
    if(g_pCbvDataBegin) g_constantBuffer->Unmap(0, nullptr);
    g_pCbvDataBegin = nullptr;
}

void ToggleFullscreen() { if (g_swapChain) { g_bFullscreen = !g_bFullscreen; g_swapChain->SetFullscreenState(g_bFullscreen, nullptr); } }

void OnResize(UINT width, UINT height) {
    if (!g_swapChain) return;
    WaitForGpuIdle();
    for (UINT i = 0; i < kFrameCount; i++) {
        g_renderTargets[i].Reset();
    }
    g_swapChain->ResizeBuffers(kFrameCount, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    
    for (UINT i = 0; i < kFrameCount; i++) {
        g_frameFenceValues[i] = g_frameFenceValues[g_frameIndex];
    }
    
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; i++) {
        g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_renderTargets[i]));
        g_device->CreateRenderTargetView(g_renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += g_rtvDescriptorSize;
    }
}

std::wstring GetResolutionType(UINT, UINT height) {
    if (height >= 2160) return L"4K (UHD)"; if (height >= 1440) return L"QHD";
    if (height >= 1080) return L"FHD"; if (height >= 720) return L"HD";
    if (height >= 480) return L"SD"; return L"";
}

HRESULT InternalCycleCamera() {
    EnterCriticalSection(&g_critSec);
    WaitForGpuIdle();
    g_bIsCameraStreamReady = false;
    if (g_pSourceReader) { g_pSourceReader.Reset(); }
    g_videoWidth = 0; g_videoHeight = 0;
    g_pCameraTextureY.Reset(); g_pCameraTextureUV.Reset();
    g_q_write_idx = 0; g_q_read_idx = 0;
    LeaveCriticalSection(&g_critSec);

    ComPtr<IMFAttributes> pAttributes;
    HRESULT hr = MFCreateAttributes(&pAttributes, 1); if(FAILED(hr)) return hr;
    hr = pAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID); if(FAILED(hr)) return hr;
    
    UINT32 count = 0;
    IMFActivate** devices = nullptr;
    hr = MFEnumDeviceSources(pAttributes.Get(), &devices, &count);
    if (FAILED(hr) || count == 0) { if(devices) CoTaskMemFree(devices); Log(L"No video capture devices found."); return E_FAIL; }

    g_currentCameraIndex = (g_currentCameraIndex + 1) % count;
    ComPtr<IMFMediaSource> pSource;
    hr = devices[g_currentCameraIndex]->ActivateObject(IID_PPV_ARGS(&pSource));
    for (UINT32 i = 0; i < count; i++) { devices[i]->Release(); }
    CoTaskMemFree(devices);
    if (FAILED(hr)) return hr;

    ComPtr<IMFAttributes> pReaderAttributes;
    hr = MFCreateAttributes(&pReaderAttributes, 1); if(FAILED(hr)) return hr;
    ComPtr<CaptureManager> pCallback;
    CaptureManager::CreateInstance(&pCallback);
    hr = pReaderAttributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, pCallback.Get()); if(FAILED(hr)) return hr;

    EnterCriticalSection(&g_critSec);
    hr = MFCreateSourceReaderFromMediaSource(pSource.Get(), pReaderAttributes.Get(), &g_pSourceReader);
    if (FAILED(hr)) { LeaveCriticalSection(&g_critSec); return hr; }

    ComPtr<IMFMediaType> pType;
    for (DWORD i = 0; SUCCEEDED(g_pSourceReader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &pType)); ++i) {
        GUID subtype; pType->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (subtype == MFVideoFormat_NV12 || subtype == MFVideoFormat_YUY2) {
            g_videoFormat = subtype; hr = g_pSourceReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, pType.Get()); break;
        } pType.Reset();
    } if (FAILED(hr)) { LeaveCriticalSection(&g_critSec); return hr; }
    
    ComPtr<IMFMediaType> pCurrentType;
    hr = g_pSourceReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCurrentType); if(FAILED(hr)) { LeaveCriticalSection(&g_critSec); return hr; }
    UINT32 width, height;
    MFGetAttributeSize(pCurrentType.Get(), MF_MT_FRAME_SIZE, &width, &height);
    g_videoWidth = width; g_videoHeight = height;

    ShaderConstants cb = { (UINT)g_videoWidth, (UINT)g_videoHeight };
    memcpy(g_pCbvDataBegin, &cb, sizeof(cb));

    size_t frameSize = 0;
    if (g_videoFormat == MFVideoFormat_YUY2) frameSize = width * height * 2;
    else if (g_videoFormat == MFVideoFormat_NV12) frameSize = width * height * 3 / 2;
    for (int i = 0; i < 3; ++i) g_cpuFrameBuffers[i].assign(frameSize, 0);

    D3D12_HEAP_PROPERTIES heapProps = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Height = g_videoHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    if (g_videoFormat == MFVideoFormat_YUY2) {
        desc.Width = g_videoWidth / 2;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        g_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&g_pCameraTextureY));
        srvDesc.Format = desc.Format;
        g_device->CreateShaderResourceView(g_pCameraTextureY.Get(), &srvDesc, srvHandle);
    } else {
        desc.Width = g_videoWidth;
        desc.Format = DXGI_FORMAT_R8_UNORM;
        g_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&g_pCameraTextureY));
        srvDesc.Format = desc.Format;
        g_device->CreateShaderResourceView(g_pCameraTextureY.Get(), &srvDesc, srvHandle);
        srvHandle.ptr += g_srvDescriptorSize;
        desc.Width = g_videoWidth / 2; desc.Height = g_videoHeight / 2; desc.Format = DXGI_FORMAT_R8G8_UNORM;
        g_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&g_pCameraTextureUV));
        srvDesc.Format = desc.Format;
        g_device->CreateShaderResourceView(g_pCameraTextureUV.Get(), &srvDesc, srvHandle);
    }
    
    hr = g_pSourceReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);
    LeaveCriticalSection(&g_critSec);
    return hr;
}

HRESULT InitializeSharing(UINT width, UINT height) {
    if (width == 0 || height == 0) return E_INVALIDARG;
    
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 1;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    g_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_sharedRtvHeap));
    
    D3D12_HEAP_PROPERTIES heapProps = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE optimizedClearValue = {};
    optimizedClearValue.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    optimizedClearValue.Color[0] = 0.0f;
    optimizedClearValue.Color[1] = 0.0f;
    optimizedClearValue.Color[2] = 0.1f;
    optimizedClearValue.Color[3] = 1.0f;
    HRESULT hr = g_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_SHARED, &texDesc, D3D12_RESOURCE_STATE_COMMON, &optimizedClearValue, IID_PPV_ARGS(&g_sharedTexture));
    if (FAILED(hr)) { LogHRESULT(L"Sharing: CreateCommittedResource for texture FAILED", hr); return hr; }
    
    g_device->CreateRenderTargetView(g_sharedTexture.Get(), nullptr, g_sharedRtvHeap->GetCPUDescriptorHandleForHeapStart());

    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, FALSE };
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd, NULL)) return E_FAIL;
    sa.lpSecurityDescriptor = sd;
    
    g_device->CreateSharedHandle(g_sharedTexture.Get(), &sa, GENERIC_ALL, g_sharedTextureName.c_str(), &g_sharedTextureHandle);
    g_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_sharedFence));
    g_device->CreateSharedHandle(g_sharedFence.Get(), &sa, GENERIC_ALL, g_sharedFenceName.c_str(), &g_sharedFenceHandle);
    
    LUID deviceLuid = g_device->GetAdapterLuid();
    ComPtr<IDXGIFactory4> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        ComPtr<IDXGIAdapter1> adapter;
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
                Log(L"New sharing session initialized successfully.");
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