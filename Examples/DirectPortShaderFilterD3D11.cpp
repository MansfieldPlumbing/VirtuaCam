// --- DirectPortShaderFilterD3D11.cpp ---
// A dynamic, auto-discovering D3D11-based filter.
// 1. Automatically finds and connects to the first available D3D11 or D3D12 producer.
// 2. Initially acts in "Passthrough" mode, re-sharing the original texture.
// 3. On pressing SPACE, allows loading a shader to switch to "Filter" mode.

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
#include <commdlg.h>
#include <tlhelp32.h>
#include <d3d12.h> // For the universal handle helper
#include "resource.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "Comdlg32.lib")

using namespace Microsoft::WRL;

// --- Logging & Structs ---
void Log(const std::wstring& msg) { WCHAR b[512]; wsprintfW(b, L"[PID:%lu][FilterD3D11] %s\n", GetCurrentProcessId(), msg.c_str()); OutputDebugStringW(b); }

struct BroadcastManifest {
    UINT64 frameValue; UINT width; UINT height; DXGI_FORMAT format;
    LUID adapterLuid; WCHAR textureName[256]; WCHAR fenceName[256];
};
struct ConstantBufferData {
    float bar_rect[4]; float resolution[2]; float time; float pad;
};
struct ProducerConnection {
    bool isConnected = false; DWORD producerPid = 0; HANDLE hManifest = nullptr;
    BroadcastManifest* pManifestView = nullptr; ComPtr<ID3D11Texture2D> sharedTexture;
    ComPtr<ID3D11Fence> sharedFence; UINT64 lastSeenFrame = 0;
    ComPtr<ID3D11Texture2D> privateTexture; ComPtr<ID3D11ShaderResourceView> privateSRV;
};

// --- D3D11 Globals ---
static ComPtr<ID3D11Device>           g_device;
static ComPtr<ID3D11Device1>          g_device1;
static ComPtr<ID3D11Device5>          g_device5;
static ComPtr<ID3D11DeviceContext>    g_context;
static ComPtr<ID3D11DeviceContext4>   g_context4;
static ComPtr<IDXGISwapChain>         g_swapChain;
static ComPtr<ID3D11RenderTargetView> g_windowRTV;
static ComPtr<ID3D11Buffer>           g_constantBuffer;
static ComPtr<ID3D11SamplerState>     g_samplerState;
static ComPtr<ID3D11VertexShader>     g_vertexShader;
static ComPtr<ID3D11PixelShader>      g_passthroughShader; // Default passthrough
static ComPtr<ID3D11PixelShader>      g_dynamicShader;     // User-loaded filter
static LUID                           g_adapterLuid = {};

// --- Input (Consumer) Globals ---
static ProducerConnection g_inputProducer;

// --- Output (Producer) Globals ---
static ComPtr<ID3D11Texture2D>        g_sharedTex_Out;
static ComPtr<ID3D11ShaderResourceView> g_sharedTexSRV_Out;
static HANDLE                       g_sharedNTHandle_Out = nullptr;
static ComPtr<ID3D11Fence>            g_sharedFence_Out;
static HANDLE                       g_sharedFenceHandle_Out = nullptr;
static UINT64                       g_frameValue_Out = 0;
static HANDLE                       g_hManifest_Out = nullptr;
static BroadcastManifest*           g_pManifestView_Out = nullptr;

// --- App State ---
static auto gStartTime = std::chrono::high_resolution_clock::now();
static float gTime = 0.0f;

// --- Shader Code ---
const char* g_vertexShaderHLSL = R"(
struct VOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
VOut main(uint vid : SV_VertexID) {
    float2 uv = float2((vid << 1) & 2, vid & 2);
    VOut o; o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1);
    o.uv = uv; return o;
})";

const char* g_passthroughShaderHLSL = R"(
Texture2D    inputTexture  : register(t0);
SamplerState linearSampler : register(s0);
struct VIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
float4 main(VIn i) : SV_Target { return inputTexture.Sample(linearSampler, i.uv); })";

// --- Forward Decls ---
static HRESULT InitD3D11(HWND hwnd);
static void LoadAssets();
static void RenderFrame(HWND hwnd);
static void FindAndConnectToProducer();
static void DisconnectFromProducer();
static HRESULT InitializeSharing(UINT width, UINT height, DXGI_FORMAT format);
static void ShutdownSharing();
static void UpdateWindowTitle();
static bool LoadShader(HWND hwnd);
static void OnResize(UINT width, UINT height);
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
HANDLE GetHandleFromName_D3D12(const WCHAR* name);

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    const WCHAR szClassName[] = L"DirectPortShaderFilterD3D11Wnd";
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEXW) };
    wcex.lpfnWndProc = WndProc; wcex.hInstance = hInstance; wcex.lpszClassName = szClassName;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(DPD_ICON1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    wcex.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(DPD_ICON1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    RegisterClassExW(&wcex);

    HWND hwnd = CreateWindowExW(0, szClassName, L"", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, nullptr, nullptr, hInstance, nullptr);
    UpdateWindowTitle();

    SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)wcex.hIcon);
    SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)wcex.hIconSm);

    if (FAILED(InitD3D11(hwnd))) { return 2; }
    LoadAssets();

    MSG msg{};
    while (msg.message != WM_QUIT) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
        } else {
            FindAndConnectToProducer();
            RenderFrame(hwnd);
        }
    }

    DisconnectFromProducer();
    return (int)msg.wParam;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_SPACE && g_inputProducer.isConnected) {
            LoadShader(hwnd);
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if (g_swapChain && wParam != SIZE_MINIMIZED) {
            OnResize(LOWORD(lParam), HIWORD(lParam));
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void OnResize(UINT width, UINT height) {
    if (!g_swapChain) return;

    g_context->OMSetRenderTargets(0, 0, 0);
    g_windowRTV.Reset();

    HRESULT hr = g_swapChain->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    if (FAILED(hr)) {
        PostQuitMessage(1);
        return;
    }

    ComPtr<ID3D11Texture2D> backBuffer;
    hr = g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (SUCCEEDED(hr)) {
        g_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_windowRTV);
    }
}

void RenderFrame(HWND hwnd) {
    if (!g_windowRTV) return; // Don't render if swap chain is resizing

    gTime = std::chrono::duration<float>(std::chrono::high_resolution_clock::now() - gStartTime).count();

    if (!g_inputProducer.isConnected) {
        const float clearColor[] = { 0.1f, 0.1f, 0.1f, 1.0f };
        g_context->ClearRenderTargetView(g_windowRTV.Get(), clearColor);
        g_swapChain->Present(1, 0);
        return;
    }
    
    // 1. Wait for a new frame from the input producer and copy it to our private texture
    UINT64 latestFrame = g_inputProducer.pManifestView->frameValue;
    if (latestFrame > g_inputProducer.lastSeenFrame) {
        g_context4->Wait(g_inputProducer.sharedFence.Get(), latestFrame);
        g_context->CopyResource(g_inputProducer.privateTexture.Get(), g_inputProducer.sharedTexture.Get());
        g_inputProducer.lastSeenFrame = latestFrame;
    }

    // 2. Render the scene to our sharable output texture
    ComPtr<ID3D11RenderTargetView> outputRTV;
    g_device->CreateRenderTargetView(g_sharedTex_Out.Get(), nullptr, &outputRTV);
    
    D3D11_TEXTURE2D_DESC outDesc;
    g_sharedTex_Out->GetDesc(&outDesc);
    D3D11_VIEWPORT vp = { 0, 0, (float)outDesc.Width, (float)outDesc.Height, 0, 1 };
    g_context->RSSetViewports(1, &vp);
    g_context->OMSetRenderTargets(1, outputRTV.GetAddressOf(), nullptr);
    const float filterClear[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    g_context->ClearRenderTargetView(outputRTV.Get(), filterClear);

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(g_context->Map(g_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        ConstantBufferData cb;
        cb.resolution[0] = (float)outDesc.Width; cb.resolution[1] = (float)outDesc.Height;
        cb.time = gTime;
        memcpy(mapped.pData, &cb, sizeof(cb));
        g_context->Unmap(g_constantBuffer.Get(), 0);
    }
    
    g_context->VSSetShader(g_vertexShader.Get(), nullptr, 0);
    g_context->PSSetShader(g_dynamicShader ? g_dynamicShader.Get() : g_passthroughShader.Get(), nullptr, 0);
    g_context->PSSetConstantBuffers(0, 1, g_constantBuffer.GetAddressOf());
    g_context->PSSetShaderResources(0, 1, g_inputProducer.privateSRV.GetAddressOf());
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->Draw(3, 0);

    // 3. Signal that our output texture is ready
    g_frameValue_Out++;
    g_context4->Signal(g_sharedFence_Out.Get(), g_frameValue_Out);
    if (g_pManifestView_Out) {
        InterlockedExchange64(reinterpret_cast<volatile LONGLONG*>(&g_pManifestView_Out->frameValue), g_frameValue_Out);
    }

    // 4. Render the output texture to the local window for preview
    RECT rc; GetClientRect(hwnd, &rc);
    vp = { 0, 0, (float)(rc.right - rc.left), (float)(rc.bottom - rc.top), 0, 1 };
    g_context->RSSetViewports(1, &vp);
    g_context->OMSetRenderTargets(1, g_windowRTV.GetAddressOf(), nullptr);
    const float previewClear[] = {0.0f, 0.0f, 0.0f, 1.0f};
    g_context->ClearRenderTargetView(g_windowRTV.Get(), previewClear);

    g_context->PSSetShaderResources(0, 1, g_sharedTexSRV_Out.GetAddressOf());
    g_context->PSSetShader(g_passthroughShader.Get(), nullptr, 0); // Always use passthrough for preview
    g_context->Draw(3, 0);

    g_swapChain->Present(1, 0);
}

void FindAndConnectToProducer() {
    static auto lastSearchTime = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    if (g_inputProducer.isConnected) {
        HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, g_inputProducer.producerPid);
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

    PROCESSENTRY32W pe32 = {}; pe32.dwSize = sizeof(PROCESSENTRY32W);
    DWORD selfPid = GetCurrentProcessId();
    const std::vector<std::wstring> producerSignatures = { L"D3D12_Producer_Manifest_", L"DirectPort_Producer_Manifest_" };

    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            if (pe32.th32ProcessID == selfPid) continue;
            
            HANDLE hManifest = nullptr;
            for (const auto& sig : producerSignatures) {
                std::wstring manifestName = sig + std::to_wstring(pe32.th32ProcessID);
                hManifest = OpenFileMappingW(FILE_MAP_READ, FALSE, manifestName.c_str());
                if (hManifest) break;
            }
            if (!hManifest) continue;

            BroadcastManifest* pManifestView = (BroadcastManifest*)MapViewOfFile(hManifest, FILE_MAP_READ, 0, 0, sizeof(BroadcastManifest));
            if (!pManifestView) { CloseHandle(hManifest); continue; }
            
            if (memcmp(&pManifestView->adapterLuid, &g_adapterLuid, sizeof(LUID)) != 0) {
                UnmapViewOfFile(pManifestView); CloseHandle(hManifest); continue;
            }

            ComPtr<ID3D11Fence> tempFence;
            HANDLE hFence = GetHandleFromName_D3D12(pManifestView->fenceName);
            if (!hFence || FAILED(g_device5->OpenSharedFence(hFence, IID_PPV_ARGS(&tempFence)))) {
                if(hFence) CloseHandle(hFence); UnmapViewOfFile(pManifestView); CloseHandle(hManifest); continue;
            }
            CloseHandle(hFence);
            
            ComPtr<ID3D11Texture2D> tempTexture;
            HANDLE hTexture = GetHandleFromName_D3D12(pManifestView->textureName);
            if (!hTexture || FAILED(g_device1->OpenSharedResource1(hTexture, IID_PPV_ARGS(&tempTexture)))) {
                if(hTexture) CloseHandle(hTexture); UnmapViewOfFile(pManifestView); CloseHandle(hManifest); continue;
            }
            CloseHandle(hTexture);

            // Connection successful, populate the struct
            g_inputProducer.producerPid = pe32.th32ProcessID;
            g_inputProducer.hManifest = hManifest;
            g_inputProducer.pManifestView = pManifestView;
            g_inputProducer.sharedFence = tempFence;
            g_inputProducer.sharedTexture = tempTexture;

            D3D11_TEXTURE2D_DESC sharedDesc;
            tempTexture->GetDesc(&sharedDesc);
            sharedDesc.MiscFlags = 0;
            sharedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            sharedDesc.Usage = D3D11_USAGE_DEFAULT;
            g_device->CreateTexture2D(&sharedDesc, nullptr, &g_inputProducer.privateTexture);
            g_device->CreateShaderResourceView(g_inputProducer.privateTexture.Get(), nullptr, &g_inputProducer.privateSRV);
            
            InitializeSharing(sharedDesc.Width, sharedDesc.Height, sharedDesc.Format);
            
            g_inputProducer.isConnected = true;
            UpdateWindowTitle();
            CloseHandle(hSnapshot);
            return;

        } while (Process32NextW(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
}

void DisconnectFromProducer() {
    if (!g_inputProducer.isConnected) return;
    Log(L"Disconnecting from producer PID: " + std::to_wstring(g_inputProducer.producerPid));
    if (g_inputProducer.pManifestView) UnmapViewOfFile(g_inputProducer.pManifestView);
    if (g_inputProducer.hManifest) CloseHandle(g_inputProducer.hManifest);
    g_inputProducer = {}; // Reset the struct
    g_dynamicShader.Reset();
    ShutdownSharing();
    UpdateWindowTitle();
}

// ... The rest of the functions are mostly copied/adapted from ShaderProducerD3D11 ...

// [The code below is very similar to DirectPortShaderProducerD3D11.cpp]

HRESULT InitD3D11(HWND hwnd) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    RECT rc; GetClientRect(hwnd, &rc);
    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount = 2; sd.BufferDesc.Width = rc.right - rc.left; sd.BufferDesc.Height = rc.bottom - rc.top; sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, nullptr, &g_context);
    if (FAILED(hr)) { return hr; }
    g_device.As(&g_device1); g_device.As(&g_device5); g_context.As(&g_context4);
    ComPtr<IDXGIDevice> dxgiDevice; g_device.As(&dxgiDevice); ComPtr<IDXGIAdapter> adapter; dxgiDevice->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC desc; adapter->GetDesc(&desc); g_adapterLuid = desc.AdapterLuid;
    
    OnResize(rc.right - rc.left, rc.bottom - rc.top);
    return S_OK;
}

void LoadAssets() {
    ComPtr<ID3DBlob> vsBlob, psBlob, err;
    D3DCompile(g_vertexShaderHLSL, strlen(g_vertexShaderHLSL), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &err);
    g_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_vertexShader);
    
    D3DCompile(g_passthroughShaderHLSL, strlen(g_passthroughShaderHLSL), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &err);
    g_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_passthroughShader);
    
    D3D11_BUFFER_DESC cbDesc = {}; cbDesc.ByteWidth = sizeof(ConstantBufferData); cbDesc.Usage = D3D11_USAGE_DYNAMIC; cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_device->CreateBuffer(&cbDesc, nullptr, &g_constantBuffer);

    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    g_device->CreateSamplerState(&sampDesc, &g_samplerState);
}

bool LoadShader(HWND hwnd) {
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd; ofn.lpstrFilter = L"Shader Files (*.cso, *.hlsl)\0*.cso;*.hlsl\0All Files\0*.*\0";
    ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) { g_dynamicShader.Reset(); UpdateWindowTitle(); return false; }

    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    DWORD size = GetFileSize(f, nullptr); std::vector<char> buffer(size);
    DWORD read = 0; ReadFile(f, buffer.data(), size, &read, nullptr); CloseHandle(f);

    ComPtr<ID3DBlob> psBlob; ComPtr<ID3DBlob> errorBlob;
    std::wstring ext = path; ext = ext.substr(ext.find_last_of(L".") + 1);
    if (_wcsicmp(ext.c_str(), L"hlsl") == 0) {
        if (FAILED(D3DCompile(buffer.data(), buffer.size(), nullptr, nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &psBlob, &errorBlob))) {
            if(errorBlob) MessageBoxA(hwnd, (char*)errorBlob->GetBufferPointer(), "HLSL Compile Error", MB_OK); return false;
        }
    } else {
        D3DCreateBlob(buffer.size(), &psBlob);
        memcpy(psBlob->GetBufferPointer(), buffer.data(), buffer.size());
    }
    
    g_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_dynamicShader);
    UpdateWindowTitle();
    return true;
}

HRESULT InitializeSharing(UINT width, UINT height, DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width; td.Height = height; td.Format = format;
    td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    HRESULT hr = g_device->CreateTexture2D(&td, nullptr, &g_sharedTex_Out);
    if (FAILED(hr)) { Log(L"Failed to create output shared texture"); return hr; }

    hr = g_device->CreateShaderResourceView(g_sharedTex_Out.Get(), nullptr, &g_sharedTexSRV_Out);
    if (FAILED(hr)) { Log(L"Failed to create SRV for output shared texture"); return hr; }

    hr = g_device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_sharedFence_Out));
    if (FAILED(hr)) { Log(L"Failed to create output shared fence"); return hr; }
    
    DWORD pid = GetCurrentProcessId();
    std::wstring textureName = L"Global\\DirectPortTexture_" + std::to_wstring(pid);
    std::wstring fenceName = L"Global\\DirectPortFence_" + std::to_wstring(pid);
    std::wstring manifestName = L"DirectPort_Producer_Manifest_" + std::to_wstring(pid);

    PSECURITY_DESCRIPTOR sd = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd, NULL);
    SECURITY_ATTRIBUTES sa{ sizeof(sa), sd, FALSE };

    ComPtr<IDXGIResource1> r1; g_sharedTex_Out.As(&r1);
    r1->CreateSharedHandle(&sa, GENERIC_ALL, textureName.c_str(), &g_sharedNTHandle_Out);
    g_sharedFence_Out->CreateSharedHandle(&sa, GENERIC_ALL, fenceName.c_str(), &g_sharedFenceHandle_Out);

    g_hManifest_Out = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(BroadcastManifest), manifestName.c_str());
    LocalFree(sd);
    if (!g_hManifest_Out) { return E_FAIL; }

    g_pManifestView_Out = (BroadcastManifest*)MapViewOfFile(g_hManifest_Out, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BroadcastManifest));
    ZeroMemory(g_pManifestView_Out, sizeof(BroadcastManifest));
    g_pManifestView_Out->width = width; g_pManifestView_Out->height = height;
    g_pManifestView_Out->format = format; g_pManifestView_Out->adapterLuid = g_adapterLuid;
    wcscpy_s(g_pManifestView_Out->textureName, textureName.c_str());
    wcscpy_s(g_pManifestView_Out->fenceName, fenceName.c_str());
    return S_OK;
}

void ShutdownSharing() {
    if (g_pManifestView_Out) UnmapViewOfFile(g_pManifestView_Out);
    if (g_hManifest_Out) CloseHandle(g_hManifest_Out);
    if (g_sharedNTHandle_Out) CloseHandle(g_sharedNTHandle_Out);
    if (g_sharedFenceHandle_Out) CloseHandle(g_sharedFenceHandle_Out);
    g_pManifestView_Out = nullptr; g_hManifest_Out = nullptr;
    g_sharedNTHandle_Out = nullptr; g_sharedFenceHandle_Out = nullptr;
    g_sharedTex_Out.Reset(); g_sharedFence_Out.Reset();
    g_sharedTexSRV_Out.Reset();
}

void UpdateWindowTitle() {
    HWND hwnd = FindWindowW(L"DirectPortShaderFilterD3D11Wnd", NULL);
    if (!hwnd) return;
    WCHAR title[512];
    if (!g_inputProducer.isConnected) {
        wsprintfW(title, L"Shader Filter (D3D11) (PID: %lu) - Searching...", GetCurrentProcessId());
    } else {
        if (g_dynamicShader) {
             wsprintfW(title, L"Shader Filter (D3D11) (PID: %lu) -> Source (PID: %lu) [FILTERING]", GetCurrentProcessId(), g_inputProducer.producerPid);
        } else {
             wsprintfW(title, L"Shader Filter (D3D11) (PID: %lu) -> Source (PID: %lu) [PASSTHROUGH]", GetCurrentProcessId(), g_inputProducer.producerPid);
        }
    }
    SetWindowTextW(hwnd, title);
}

HANDLE GetHandleFromName_D3D12(const WCHAR* name) {
    ComPtr<ID3D12Device> d3d12Device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12Device)))) return NULL;
    HANDLE handle = nullptr;
    d3d12Device->OpenSharedHandleByName(name, GENERIC_ALL, &handle);
    return handle;
}