#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl.h>
#include <sddl.h>
#include <string>
#include <chrono>
#include <vector>
#include <cmath>
#include <memory>
#include <tlhelp32.h>
#include <algorithm>
#include <d3d12.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "d3d12.lib") 

#include "resource.h"

using namespace Microsoft::WRL;

void Log(const std::wstring& msg) {
    WCHAR buffer[1024];
    DWORD pid = GetCurrentProcessId();
    wsprintfW(buffer, L"[PID:%lu][Multiplexer] %s\n", pid, msg.c_str());
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

static ComPtr<ID3D11Device>           g_device;
static ComPtr<ID3D11Device1>          g_device1;
static ComPtr<ID3D11Device5>          g_device5;
static ComPtr<ID3D11DeviceContext>    g_context;
static ComPtr<ID3D11DeviceContext4>   g_context4;
static ComPtr<IDXGISwapChain>         g_swapChain;
static ComPtr<ID3D11RenderTargetView> g_windowRTV; 
static ComPtr<ID3D11VertexShader>     g_vertexShader;
static ComPtr<ID3D11PixelShader>      g_pixelShader;
static ComPtr<ID3D11SamplerState>     g_samplerState;
static HWND                           g_hwnd = nullptr;
static LUID                           g_adapterLuid = {};

static const UINT MUX_WIDTH = 1280;
static const UINT MUX_HEIGHT = 720;
static ComPtr<ID3D11Texture2D>        g_compositeTexture;     
static ComPtr<ID3D11RenderTargetView> g_compositeRTV;         
static ComPtr<ID3D11ShaderResourceView> g_compositeSRV;
static ComPtr<ID3D11Texture2D>        g_sharedOutTexture;     
static ComPtr<ID3D11Fence>            g_sharedOutFence;       
static UINT64                         g_sharedOutFrameValue = 0;
static HANDLE                         g_hManifestOut = nullptr;
static BroadcastManifest*             g_pManifestViewOut = nullptr;
static HANDLE                         g_sharedOutTextureHandle = nullptr;
static HANDLE                         g_sharedOutFenceHandle = nullptr;

const int MAX_PRODUCERS = 256; 
struct ProducerConnection {
    bool isConnected = false;
    DWORD producerPid = 0;
    HANDLE hManifest = nullptr;
    BroadcastManifest* pManifestView = nullptr;
    ComPtr<ID3D11Texture2D> sharedTexture;
    ComPtr<ID3D11Fence> sharedFence;
    UINT64 lastSeenFrame = 0;
    ComPtr<ID3D11Texture2D> privateTexture;
    ComPtr<ID3D11ShaderResourceView> privateSRV;
};
static ProducerConnection g_producers[MAX_PRODUCERS];

void InitD3D11(HWND hwnd);
void LoadAssets();
void RenderFrame();
void FindAndConnectToProducers();
void DisconnectFromProducer(int producerIndex);
void UpdateWindowTitle();
HRESULT InitializeSharing();
void Cleanup();
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
HANDLE GetHandleFromName_D3D12(const WCHAR* name);

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
    const WCHAR szClassName[] = L"DirectPortMultiplexerWindowClass";
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEXW) };
    wcex.lpfnWndProc = WndProc; wcex.hInstance = hInstance; wcex.lpszClassName = szClassName;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(DPD_ICON1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    wcex.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(DPD_ICON1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    RegisterClassExW(&wcex);

    g_hwnd = CreateWindowExW(0, szClassName, L"DirectPort Multiplexer (D3D11)", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, MUX_WIDTH, MUX_HEIGHT, nullptr, nullptr, hInstance, nullptr);

    SendMessage(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)wcex.hIcon);
    SendMessage(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)wcex.hIconSm);

    InitD3D11(g_hwnd);
    LoadAssets();
    InitializeSharing();
    ShowWindow(g_hwnd, nCmdShow);

    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessage(&msg);
        } else {
            FindAndConnectToProducers();
            RenderFrame();
        }
    }
    Cleanup();
    return static_cast<int>(msg.wParam);
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

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
        D3D11_SDK_VERSION, &scd, &g_swapChain, &g_device, nullptr, &g_context);

    g_device.As(&g_device5);
    g_device.As(&g_device1);
    g_context.As(&g_context4);

    ComPtr<ID3D11Texture2D> backBuffer;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    g_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_windowRTV);

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
    g_device->CreateSamplerState(&sampDesc, &g_samplerState);
}

HRESULT InitializeSharing() {
    D3D11_TEXTURE2D_DESC compositeDesc = {};
    compositeDesc.Width = MUX_WIDTH;
    compositeDesc.Height = MUX_HEIGHT;
    compositeDesc.MipLevels = 1;
    compositeDesc.ArraySize = 1;
    compositeDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    compositeDesc.SampleDesc.Count = 1;
    compositeDesc.Usage = D3D11_USAGE_DEFAULT;
    compositeDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    g_device->CreateTexture2D(&compositeDesc, nullptr, &g_compositeTexture);
    g_device->CreateRenderTargetView(g_compositeTexture.Get(), nullptr, &g_compositeRTV);
    g_device->CreateShaderResourceView(g_compositeTexture.Get(), nullptr, &g_compositeSRV);

    D3D11_TEXTURE2D_DESC sharedDesc = compositeDesc;
    sharedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sharedDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    HRESULT hr = g_device->CreateTexture2D(&sharedDesc, nullptr, &g_sharedOutTexture);
    if (FAILED(hr)) { Log(L"Failed to create shared output texture."); return hr; }

    hr = g_device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_sharedOutFence));
    if (FAILED(hr)) { Log(L"Failed to create shared output fence."); return hr; }
    
    DWORD pid = GetCurrentProcessId();
    std::wstring manifestName = L"DirectPort_Producer_Manifest_" + std::to_wstring(pid);
    std::wstring textureName = L"Global\\DirectPortTexture_" + std::to_wstring(pid);
    std::wstring fenceName = L"Global\\DirectPortFence_" + std::to_wstring(pid);

    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd, NULL)) return E_FAIL;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd, FALSE };

    ComPtr<IDXGIResource1> resource1;
    g_sharedOutTexture.As(&resource1);
    
    resource1->CreateSharedHandle(&sa, GENERIC_ALL, textureName.c_str(), &g_sharedOutTextureHandle);
    g_sharedOutFence->CreateSharedHandle(&sa, GENERIC_ALL, fenceName.c_str(), &g_sharedOutFenceHandle);

    g_hManifestOut = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(BroadcastManifest), manifestName.c_str());
    LocalFree(sd);
    if (g_hManifestOut == NULL) { Log(L"CreateFileMappingW failed for output."); return E_FAIL; }

    g_pManifestViewOut = (BroadcastManifest*)MapViewOfFile(g_hManifestOut, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BroadcastManifest));
    if (g_pManifestViewOut == nullptr) { Log(L"MapViewOfFile failed for output."); return E_FAIL; }

    ZeroMemory(g_pManifestViewOut, sizeof(BroadcastManifest));
    g_pManifestViewOut->width = MUX_WIDTH;
    g_pManifestViewOut->height = MUX_HEIGHT;
    g_pManifestViewOut->format = DXGI_FORMAT_B8G8R8A8_UNORM;
    g_pManifestViewOut->adapterLuid = g_adapterLuid;
    wcscpy_s(g_pManifestViewOut->textureName, textureName.c_str());
    wcscpy_s(g_pManifestViewOut->fenceName, fenceName.c_str());

    Log(L"Multiplexer sharing initialized successfully.");
    return S_OK;
}

void RenderFrame() {
    for (int i = 0; i < MAX_PRODUCERS; ++i) {
        auto& producer = g_producers[i];
        if (producer.isConnected && producer.pManifestView) {
            UINT64 latestFrame = producer.pManifestView->frameValue;
            if (latestFrame > producer.lastSeenFrame) {
                g_context4->Wait(producer.sharedFence.Get(), latestFrame);
                g_context->CopyResource(producer.privateTexture.Get(), producer.sharedTexture.Get());
                producer.lastSeenFrame = latestFrame;
            }
        }
    }

    g_context->OMSetRenderTargets(1, g_compositeRTV.GetAddressOf(), nullptr);
    const float compositeClearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f }; 
    g_context->ClearRenderTargetView(g_compositeRTV.Get(), compositeClearColor);
    
    g_context->VSSetShader(g_vertexShader.Get(), nullptr, 0);
    g_context->PSSetShader(g_pixelShader.Get(), nullptr, 0);
    g_context->PSSetSamplers(0, 1, g_samplerState.GetAddressOf());
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    std::vector<int> activeIndices;
    for (int i = 0; i < MAX_PRODUCERS; ++i) {
        if (g_producers[i].isConnected && g_producers[i].privateSRV) {
            activeIndices.push_back(i);
        }
    }
    
    int connectedCount = activeIndices.size();
    if (connectedCount > 0) {
        int cols = static_cast<int>(ceil(sqrt(static_cast<float>(connectedCount))));
        int rows = (connectedCount + cols - 1) / cols;

        for (int i = 0; i < connectedCount; ++i) {
            int producerIndex = activeIndices[i];
            int gridCol = i % cols;
            int gridRow = i / cols;

            int left   = (gridCol * MUX_WIDTH) / cols;
            int right  = ((gridCol + 1) * MUX_WIDTH) / cols;
            int top    = (gridRow * MUX_HEIGHT) / rows;
            int bottom = ((gridRow + 1) * MUX_HEIGHT) / rows;

            D3D11_VIEWPORT vp = {};
            vp.TopLeftX = static_cast<float>(left);
            vp.TopLeftY = static_cast<float>(top);
            vp.Width    = static_cast<float>(right - left);
            vp.Height   = static_cast<float>(bottom - top);
            vp.MinDepth = 0.0f;
            vp.MaxDepth = 1.0f;
            
            g_context->RSSetViewports(1, &vp);
            
            g_context->PSSetShaderResources(0, 1, g_producers[producerIndex].privateSRV.GetAddressOf());
            g_context->Draw(3, 0);
        }
    }

    g_context->CopyResource(g_sharedOutTexture.Get(), g_compositeTexture.Get());
    g_sharedOutFrameValue++;
    g_context4->Signal(g_sharedOutFence.Get(), g_sharedOutFrameValue);
    if(g_pManifestViewOut) {
         InterlockedExchange64(reinterpret_cast<volatile LONGLONG*>(&g_pManifestViewOut->frameValue), g_sharedOutFrameValue);
    }
    
    g_context->OMSetRenderTargets(1, g_windowRTV.GetAddressOf(), nullptr);
    const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
    g_context->ClearRenderTargetView(g_windowRTV.Get(), clearColor);

    RECT clientRect;
    GetClientRect(g_hwnd, &clientRect);
    D3D11_VIEWPORT presentVp = {};
    presentVp.Width    = static_cast<float>(clientRect.right - clientRect.left);
    presentVp.Height   = static_cast<float>(clientRect.bottom - clientRect.top);
    presentVp.MinDepth = 0.0f;
    presentVp.MaxDepth = 1.0f;
    g_context->RSSetViewports(1, &presentVp);
    
    g_context->PSSetShaderResources(0, 1, g_compositeSRV.GetAddressOf());
    g_context->Draw(3, 0);

    g_swapChain->Present(1, 0);
}


void FindAndConnectToProducers() {
    static auto lastSearchTime = std::chrono::steady_clock::now() - std::chrono::seconds(2);

    for (int i = 0; i < MAX_PRODUCERS; ++i) {
        auto& producer = g_producers[i];
        if (!producer.isConnected) continue;
        HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, producer.producerPid);
        if (hProcess == NULL || WaitForSingleObject(hProcess, 0) != WAIT_TIMEOUT) {
            DisconnectFromProducer(i);
        }
        if (hProcess) CloseHandle(hProcess);
    }

    if (std::chrono::steady_clock::now() - lastSearchTime < std::chrono::seconds(1)) return;
    lastSearchTime = std::chrono::steady_clock::now();

    int availableSlot = -1;
    for (int i = 0; i < MAX_PRODUCERS; ++i) if (!g_producers[i].isConnected) { availableSlot = i; break; }
    if (availableSlot == -1) return;

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe32 = {}; pe32.dwSize = sizeof(PROCESSENTRY32W);
    DWORD selfPid = GetCurrentProcessId();
    const std::vector<std::wstring> producerSignatures = { L"DirectPort_Producer_Manifest_", L"D3D12_Producer_Manifest_" };

    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            if (pe32.th32ProcessID == selfPid) continue;

            bool alreadyConnected = false;
            for (int i = 0; i < MAX_PRODUCERS; ++i) {
                if (g_producers[i].isConnected && g_producers[i].producerPid == pe32.th32ProcessID) {
                    alreadyConnected = true;
                    break;
                }
            }
            if (alreadyConnected) continue;

            for (const auto& sig : producerSignatures) {
                std::wstring manifestName = sig + std::to_wstring(pe32.th32ProcessID);
                HANDLE hManifest = OpenFileMappingW(FILE_MAP_READ, FALSE, manifestName.c_str());
                if (!hManifest) continue;

                BroadcastManifest* pManifestView = (BroadcastManifest*)MapViewOfFile(hManifest, FILE_MAP_READ, 0, 0, sizeof(BroadcastManifest));
                if (!pManifestView) { CloseHandle(hManifest); continue; }
                
                bool luidMatch = (memcmp(&pManifestView->adapterLuid, &g_adapterLuid, sizeof(LUID)) == 0);
                auto& producer = g_producers[availableSlot];
                HRESULT hr;
                
                HANDLE hFence = GetHandleFromName_D3D12(pManifestView->fenceName);
                if (!hFence) { UnmapViewOfFile(pManifestView); CloseHandle(hManifest); continue; }
                hr = g_device5->OpenSharedFence(hFence, IID_PPV_ARGS(&producer.sharedFence));
                CloseHandle(hFence);
                if (FAILED(hr)) { UnmapViewOfFile(pManifestView); CloseHandle(hManifest); continue; }

                hr = g_device1->OpenSharedResourceByName(pManifestView->textureName, DXGI_SHARED_RESOURCE_READ, IID_PPV_ARGS(&producer.sharedTexture));
                if (FAILED(hr)) { 
                    HANDLE hTexture = GetHandleFromName_D3D12(pManifestView->textureName);
                    if (!hTexture) { UnmapViewOfFile(pManifestView); CloseHandle(hManifest); continue; }
                    hr = g_device1->OpenSharedResource1(hTexture, IID_PPV_ARGS(&producer.sharedTexture));
                    CloseHandle(hTexture);
                    if (FAILED(hr)) { UnmapViewOfFile(pManifestView); CloseHandle(hManifest); continue; }
                }
                
                if (!luidMatch) {
                    Log(L"Warning: Connecting to producer PID " + std::to_wstring(pe32.th32ProcessID) + L" on a different GPU adapter. Performance may vary.");
                }

                D3D11_TEXTURE2D_DESC sharedDesc;
                producer.sharedTexture->GetDesc(&sharedDesc);
                sharedDesc.MiscFlags = 0;
                sharedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                sharedDesc.Usage = D3D11_USAGE_DEFAULT;
                hr = g_device->CreateTexture2D(&sharedDesc, nullptr, &producer.privateTexture);
                if (FAILED(hr)) { UnmapViewOfFile(pManifestView); CloseHandle(hManifest); continue; }
                
                hr = g_device->CreateShaderResourceView(producer.privateTexture.Get(), nullptr, &producer.privateSRV);
                if (FAILED(hr)) { UnmapViewOfFile(pManifestView); CloseHandle(hManifest); continue; }
                
                producer.producerPid = pe32.th32ProcessID;
                producer.hManifest = hManifest;
                producer.pManifestView = pManifestView;
                producer.isConnected = true;
                producer.lastSeenFrame = 0;
                Log(L"Successfully connected to producer PID: " + std::to_wstring(pe32.th32ProcessID));

                UpdateWindowTitle();
                availableSlot = -1;
                for (int i = 0; i < MAX_PRODUCERS; ++i) if (!g_producers[i].isConnected) { availableSlot = i; break; }
                
                if (availableSlot == -1) { CloseHandle(hSnapshot); return; } 
                break; 
            }
        } while (Process32NextW(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
}

void DisconnectFromProducer(int producerIndex) {
    auto& producer = g_producers[producerIndex];
    if (!producer.isConnected) return;
    Log(L"Disconnecting from producer PID: " + std::to_wstring(producer.producerPid));
    producer.isConnected = false;
    producer.producerPid = 0;
    if (producer.pManifestView) UnmapViewOfFile(producer.pManifestView);
    if (producer.hManifest) CloseHandle(producer.hManifest);
    producer.pManifestView = nullptr;
    producer.hManifest = nullptr;
    producer.sharedTexture.Reset();
    producer.sharedFence.Reset();
    producer.privateTexture.Reset();
    producer.privateSRV.Reset();
    UpdateWindowTitle();
}

void UpdateWindowTitle() {
    std::wstring title = L"DirectPort Multiplexer (D3D11) - ";
    int connectedCount = 0;
    for (int i = 0; i < MAX_PRODUCERS; ++i) if (g_producers[i].isConnected) connectedCount++;
    if (connectedCount == 0) {
        title += L"Searching... | ";
    } else {
        title += L"Consuming " + std::to_wstring(connectedCount) + L" stream(s) | ";
    }
    title += L"Producing (PID: " + std::to_wstring(GetCurrentProcessId()) + L")";
    SetWindowTextW(g_hwnd, title.c_str());
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
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

void Cleanup() {
    for(int i = 0; i < MAX_PRODUCERS; ++i) {
        DisconnectFromProducer(i);
    }
    if (g_pManifestViewOut) UnmapViewOfFile(g_pManifestViewOut);
    if (g_hManifestOut) CloseHandle(g_hManifestOut);

    if (g_sharedOutTextureHandle) CloseHandle(g_sharedOutTextureHandle);
    if (g_sharedOutFenceHandle) CloseHandle(g_sharedOutFenceHandle);
}