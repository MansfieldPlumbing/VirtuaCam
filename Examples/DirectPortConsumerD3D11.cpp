#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h> 
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl.h>
#include <string>
#include <chrono>
#include <tlhelp32.h>
#include <algorithm>
#include <cwctype>
#include <vector>
#include <d3d12.h> // Required for the universal handle lookup helper
#include "resource.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "d3d12.lib") // Required for the universal handle lookup helper

using namespace Microsoft::WRL;

// --- Logging and Manifest ---
void Log(const std::wstring& msg) {
    WCHAR buffer[1024];
    DWORD pid = GetCurrentProcessId();
    wsprintfW(buffer, L"[PID:%lu][DirectPortConsumer] %s\n", pid, msg.c_str());
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

// --- D3D11 Globals ---
static ComPtr<ID3D11Device>           g_device;
static ComPtr<ID3D11Device1>          g_device1;
static ComPtr<ID3D11Device5>          g_device5;
static ComPtr<ID3D11DeviceContext>    g_context;
static ComPtr<ID3D11DeviceContext4>   g_context4;
static ComPtr<IDXGISwapChain>         g_swapChain;
static ComPtr<ID3D11RenderTargetView> g_rtv;
static ComPtr<ID3D11VertexShader>     g_vertexShader;
static ComPtr<ID3D11PixelShader>      g_pixelShader;
static ComPtr<ID3D11SamplerState>     g_samplerState;
static LUID                           g_adapterLuid = {};

const int MAX_PRODUCERS = 1;

struct ProducerConnection {
    bool                           isConnected = false;
    DWORD                          producerPid = 0;
    HANDLE                         hManifest = nullptr;
    BroadcastManifest*             pManifestView = nullptr;
    ComPtr<ID3D11Texture2D>        sharedTexture;
    ComPtr<ID3D11Fence>            sharedFence;      
    UINT64                         lastSeenFrame = 0;
    ComPtr<ID3D11ShaderResourceView> sharedSRV;
    ComPtr<ID3D11Texture2D>        privateTexture;
    ComPtr<ID3D11ShaderResourceView> privateSRV;
    UINT                           connectedTextureWidth = 0;
    UINT                           connectedTextureHeight = 0;
    std::wstring                   producerType;
};

static ProducerConnection g_producers[MAX_PRODUCERS];

void InitD3D11(HWND hwnd);
void LoadAssets();
void RenderFrame();
void FindAndConnectToProducers();
void DisconnectFromProducer(int producerIndex);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void CreatePrivateTextureAndSRV(int producerIndex, UINT width, UINT height, DXGI_FORMAT format);
void UpdateWindowTitle();
HANDLE GetHandleFromName_D3D12(const WCHAR* name);
// --- FIX: Add forward declaration for OnResize ---
void OnResize(UINT width, UINT height);

const char* g_vertexShaderHLSL = R"(
    struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
    PSInput VSMain(uint id : SV_VertexID) {
        PSInput result;
        float2 uv = float2((id << 1) & 2, id & 2);
        result.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1);
        result.uv = uv;
        return result;
    }
)";
const char* g_pixelShaderHLSL = R"(
    Texture2D g_texture : register(t0);
    SamplerState g_sampler : register(s0);
    struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
    float4 PSMain(PSInput input) : SV_TARGET {
        return g_texture.Sample(g_sampler, input.uv);
    }
)";

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    const WCHAR szClassName[] = L"DirectPortConsumerWindowClass";

    WNDCLASSEXW wcex = { sizeof(WNDCLASSEXW) };
    wcex.lpfnWndProc   = WndProc;
    wcex.hInstance     = hInstance;
    wcex.lpszClassName = szClassName;
    wcex.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wcex.hIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(DPD_ICON1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    wcex.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(DPD_ICON1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    
    RegisterClassExW(&wcex);

    HWND hwnd = CreateWindowExW(0, szClassName, L"DirectPort Consumer - Searching...", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) return 1;

    SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)wcex.hIcon);
    SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)wcex.hIconSm);

    InitD3D11(hwnd);
    LoadAssets();
    ShowWindow(hwnd, nCmdShow);

    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            FindAndConnectToProducers();
            RenderFrame();
        }
    }

    for (int i = 0; i < MAX_PRODUCERS; ++i) {
        DisconnectFromProducer(i);
    }
    return static_cast<int>(msg.wParam);
}

// --- FIX: Add a proper WndProc to handle WM_SIZE events ---
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


void InitD3D11(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, creationFlags, nullptr, 0,
        D3D11_SDK_VERSION, &scd, &g_swapChain, &g_device, nullptr, &g_context);

    if (FAILED(g_device.As(&g_device5)) || FAILED(g_context.As(&g_context4))) {
        Log(L"Fatal: Could not query for ID3D11Device5 or ID3D11DeviceContext4. Fence synchronization is not supported.");
        MessageBox(hwnd, L"Fence synchronization is not supported on this device.", L"Error", MB_OK);
        PostQuitMessage(1);
    }
    g_device.As(&g_device1);

    ComPtr<ID3D11Texture2D> backBuffer;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    g_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_rtv);

    ComPtr<IDXGIDevice> dxgiDevice;
    g_device.As(&dxgiDevice);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC desc;
    adapter->GetDesc(&desc);
    g_adapterLuid = desc.AdapterLuid;
}

void LoadAssets() {
    ComPtr<ID3DBlob> vsBlob, psBlob;
    D3DCompile(g_vertexShaderHLSL, strlen(g_vertexShaderHLSL), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsBlob, nullptr);
    D3DCompile(g_pixelShaderHLSL, strlen(g_pixelShaderHLSL), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psBlob, nullptr);

    g_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_vertexShader);
    g_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_pixelShader);

    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    g_device->CreateSamplerState(&sampDesc, &g_samplerState);
}

void CreatePrivateTextureAndSRV(int producerIndex, UINT width, UINT height, DXGI_FORMAT format) {
    if (producerIndex < 0 || producerIndex >= MAX_PRODUCERS) return;
    
    auto& producer = g_producers[producerIndex];
    producer.privateTexture.Reset();
    producer.privateSRV.Reset();

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format; 
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    
    if (SUCCEEDED(g_device->CreateTexture2D(&desc, nullptr, &producer.privateTexture))) {
        g_device->CreateShaderResourceView(producer.privateTexture.Get(), nullptr, &producer.privateSRV);
    } else {
        Log(L"Failed to create private texture or SRV for producer " + std::to_wstring(producerIndex));
    }
}

void RenderFrame() {
    if (!g_rtv) return; // RTV can be null during a resize operation

    g_context->OMSetRenderTargets(1, g_rtv.GetAddressOf(), nullptr);

    const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f }; 
    g_context->ClearRenderTargetView(g_rtv.Get(), clearColor);

    RECT clientRect;
    GetClientRect(FindWindowW(L"DirectPortConsumerWindowClass", NULL), &clientRect);
    float windowWidth = (float)(clientRect.right - clientRect.left);
    float windowHeight = (float)(clientRect.bottom - clientRect.top);

    for (int i = 0; i < MAX_PRODUCERS; ++i) {
        auto& producer = g_producers[i];

        if (producer.isConnected && producer.sharedSRV && producer.privateSRV) {
            
            UINT64 latestFrame = producer.pManifestView->frameValue;
            if (latestFrame > producer.lastSeenFrame) {
                g_context4->Wait(producer.sharedFence.Get(), latestFrame);
                g_context->CopyResource(producer.privateTexture.Get(), producer.sharedTexture.Get());
                producer.lastSeenFrame = latestFrame;
            }
            
            // --- FIX: Use the full window viewport, not a split-screen one ---
            D3D11_VIEWPORT vp = {};
            vp.Width = windowWidth;
            vp.Height = windowHeight;
            vp.TopLeftX = 0.0f;
            vp.TopLeftY = 0.0f;
            vp.MinDepth = 0.0f;
            vp.MaxDepth = 1.0f;
            g_context->RSSetViewports(1, &vp);

            g_context->VSSetShader(g_vertexShader.Get(), nullptr, 0);
            g_context->PSSetShader(g_pixelShader.Get(), nullptr, 0);
            g_context->PSSetShaderResources(0, 1, producer.privateSRV.GetAddressOf());
            g_context->PSSetSamplers(0, 1, g_samplerState.GetAddressOf());
            g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_context->Draw(3, 0);
        }
    }

    g_swapChain->Present(1, 0);
}

// --- FIX: Add OnResize function to handle swap chain recreation ---
void OnResize(UINT width, UINT height) {
    if (g_swapChain) {
        // Ensure all commands are flushed before resizing
        g_context->Flush();

        // Release the old render target view
        g_rtv.Reset();

        // Resize the swap chain
        HRESULT hr = g_swapChain->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        if (FAILED(hr)) {
            Log(L"Failed to resize swap chain.");
            // Handle error, maybe exit or try to recover
            return;
        }

        // Get the new back buffer and create a new render target view
        ComPtr<ID3D11Texture2D> backBuffer;
        hr = g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (SUCCEEDED(hr)) {
            g_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_rtv);
        }
    }
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
           (producer.pManifestView->width != producer.connectedTextureWidth || producer.pManifestView->height != producer.connectedTextureHeight)) {
            Log(L"Producer " + std::to_wstring(i) + L" resolution changed. Reconnecting...");
            needsDisconnect = true;
        }

        if (needsDisconnect) {
            DisconnectFromProducer(i);
        }
    }

    if (std::chrono::steady_clock::now() - lastSearchTime < std::chrono::seconds(1)) {
        return;
    }
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

            manifestName = D3D12_PRODUCER_MANIFEST_PREFIX + std::to_wstring(pe32.th32ProcessID);
            HANDLE hManifest = OpenFileMappingW(FILE_MAP_READ, FALSE, manifestName.c_str());

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
            if (!pManifestView) { CloseHandle(hManifest); continue; }
            
            if (memcmp(&pManifestView->adapterLuid, &g_adapterLuid, sizeof(LUID)) == 0) {
                ComPtr<ID3D11Texture2D> tempTexture;
                ComPtr<ID3D11Fence> tempFence;
                ComPtr<ID3D11ShaderResourceView> tempSRV;

                HANDLE hFence = GetHandleFromName_D3D12(pManifestView->fenceName);
                if (hFence == NULL) {
                    Log(L"Failed to get handle for fence: " + std::wstring(pManifestView->fenceName));
                    UnmapViewOfFile(pManifestView); CloseHandle(hManifest); continue;
                }

                HRESULT hr = g_device5->OpenSharedFence(hFence, IID_PPV_ARGS(&tempFence));
                CloseHandle(hFence); 
                if (FAILED(hr)) { Log(L"OpenSharedFence failed."); UnmapViewOfFile(pManifestView); CloseHandle(hManifest); continue; }

                hr = g_device1->OpenSharedResourceByName(pManifestView->textureName, DXGI_SHARED_RESOURCE_READ, IID_PPV_ARGS(&tempTexture));
                if (FAILED(hr)) { Log(L"OpenSharedResourceByName failed."); UnmapViewOfFile(pManifestView); CloseHandle(hManifest); continue; }

                hr = g_device->CreateShaderResourceView(tempTexture.Get(), nullptr, &tempSRV);
                if (FAILED(hr)) { Log(L"CreateShaderResourceView failed."); UnmapViewOfFile(pManifestView); CloseHandle(hManifest); continue; }

                auto& producer = g_producers[availableSlot];
                producer.producerPid = pe32.th32ProcessID;
                producer.hManifest = hManifest;
                producer.pManifestView = pManifestView;
                producer.isConnected = true;
                producer.sharedTexture = tempTexture;
                producer.sharedFence = tempFence;
                producer.sharedSRV = tempSRV;
                producer.lastSeenFrame = 0;
                producer.producerType = currentProducerType;

                D3D11_TEXTURE2D_DESC desc;
                producer.sharedTexture->GetDesc(&desc);
                producer.connectedTextureWidth = desc.Width;
                producer.connectedTextureHeight = desc.Height;
                
                CreatePrivateTextureAndSRV(availableSlot, desc.Width, desc.Height, pManifestView->format);
                
                Log(L"Slot " + std::to_wstring(availableSlot) + L": Connected to " + currentProducerType + L" (PID: " + std::to_wstring(pe32.th32ProcessID) + L")");
                UpdateWindowTitle();
                
                CloseHandle(hSnapshot);
                return;
            }
            
            UnmapViewOfFile(pManifestView);
            CloseHandle(hManifest);

        } while (Process32NextW(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
}

void DisconnectFromProducer(int producerIndex) {
    if (producerIndex < 0 || producerIndex >= MAX_PRODUCERS || !g_producers[producerIndex].isConnected) return;
    
    Log(L"Disconnecting producer in slot " + std::to_wstring(producerIndex));

    auto& producer = g_producers[producerIndex];
    producer.isConnected = false;
    producer.producerPid = 0;
    if (producer.pManifestView) UnmapViewOfFile(producer.pManifestView);
    if (producer.hManifest) CloseHandle(producer.hManifest);
    producer.pManifestView = nullptr;
    producer.hManifest = nullptr;
    producer.sharedTexture.Reset();
    producer.sharedFence.Reset();
    producer.sharedSRV.Reset();
    producer.privateTexture.Reset();
    producer.privateSRV.Reset();
    producer.connectedTextureWidth = 0;
    producer.connectedTextureHeight = 0;
    producer.producerType.clear();

    UpdateWindowTitle();
}

void UpdateWindowTitle() {
    std::wstring title = L"DirectPort Consumer - ";
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
    
    SetWindowTextW(FindWindowW(L"DirectPortConsumerWindowClass", NULL), title.c_str());
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