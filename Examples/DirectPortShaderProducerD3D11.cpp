// --- DirectPortShaderProducerD3D11.cpp ---
// A DirectPort producer that can load a pixel shader from a .cso or .hlsl file
// and broadcast the rendered result as a shared texture.
// Press SPACE to open a file dialog.
//
// This version contains fixes for:
// 1. A GPU crash caused by re-rendering the scene for preview.
// 2. Implements a correct single-render pass with a blit for the preview window.
// 3. Adds the missing LogHRESULT function to fix build errors.

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl.h>
#include <sddl.h>
#include <string>
#include <chrono>
#include <vector>
#include <commdlg.h>
#include "resource.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"user32.lib")
#pragma comment(lib,"d3dcompiler.lib")
#pragma comment(lib,"advapi32.lib")
#pragma comment(lib,"Comdlg32.lib")

using Microsoft::WRL::ComPtr;

// --- Logging ---
void Log(const std::wstring& msg) { WCHAR b[512]; wsprintfW(b, L"[PID:%lu][ShaderProducerD3D11] %s\n", GetCurrentProcessId(), msg.c_str()); OutputDebugStringW(b); }
// --- FIX: Add the missing LogHRESULT function definition ---
void LogHRESULT(const std::wstring& msg, HRESULT hr) { WCHAR b[512]; wsprintfW(b, L"[PID:%lu][ShaderProducerD3D11] %s - HRESULT: 0x%08X\n", GetCurrentProcessId(), msg.c_str(), hr); OutputDebugStringW(b); }
// --- END OF FIX ---

// --- Core Structs ---
struct BroadcastManifest {
    UINT64 frameValue; UINT width; UINT height; DXGI_FORMAT format;
    LUID adapterLuid; WCHAR textureName[256]; WCHAR fenceName[256];
};

struct ConstantBufferData {
    float bar_rect[4];
    float resolution[2];
    float time;
    float pad;
};

// --- Globals ---
static ComPtr<ID3D11Device>           gDev;
static ComPtr<ID3D11Device5>          gDev5;
static ComPtr<ID3D11DeviceContext>    gCtx;
static ComPtr<ID3D11DeviceContext4>   gCtx4;
static ComPtr<IDXGISwapChain>         gSwap;
static ComPtr<ID3D11RenderTargetView> gWindowRTV;
static ComPtr<ID3D11Buffer>           gConstantBuffer;
static ComPtr<ID3D11VertexShader>     gVertexShader;
static ComPtr<ID3D11PixelShader>      gDynamicPixelShader;
static ComPtr<ID3D11Texture2D>        gPrivateTex;
static ComPtr<ID3D11RenderTargetView> gPrivateTexRTV;

static ComPtr<ID3D11PixelShader>      gPassthroughPS;
static ComPtr<ID3D11SamplerState>     gSamplerState;
static ComPtr<ID3D11ShaderResourceView> gSharedTexSRV;

static std::wstring                   gSharedTextureName, gSharedFenceName;
static ComPtr<ID3D11Texture2D>        gSharedTex;
static HANDLE                       gSharedNTHandle = nullptr;
static ComPtr<ID3D11Fence>            gSharedFence;
static HANDLE                       gSharedFenceHandle = nullptr;
static UINT64                       gFrameValue = 0;
static HANDLE                       g_hManifest = nullptr;
static BroadcastManifest*           g_pManifestView = nullptr;

static auto gStartTime = std::chrono::high_resolution_clock::now();
static float gTime = 0.0f;
static UINT RENDER_W = 1280, RENDER_H = 720;

const char* g_VertexShaderHLSL = R"(
struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD; };
VOut main(uint vid : SV_VertexID) {
    float2 uv = float2((vid << 1) & 2, vid & 2);
    VOut o; o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1);
    o.uv = uv; return o;
})";

const char* g_PassthroughPixelShaderHLSL = R"(
Texture2D    g_texture : register(t0);
SamplerState g_sampler : register(s0);
struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
float4 main(PSInput input) : SV_TARGET { return g_texture.Sample(g_sampler, input.uv); })";

// Forward Declarations
static HRESULT CreateDeviceAndResources(HWND hwnd);
static HRESULT CreateSharedTexture();
static HRESULT CreateSharedFence();
static HRESULT CreateBroadcastManifest();
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
static bool LoadShader(HWND hwnd);
static void OnResize(UINT width, UINT height);

void RenderFrame(HWND hwnd) {
    if (!gWindowRTV) return;

    gTime = std::chrono::duration<float>(std::chrono::high_resolution_clock::now() - gStartTime).count();

    gCtx->OMSetRenderTargets(1, gPrivateTexRTV.GetAddressOf(), nullptr);
    D3D11_VIEWPORT vp = { 0, 0, (float)RENDER_W, (float)RENDER_H, 0, 1 };
    gCtx->RSSetViewports(1, &vp);
    
    const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
    gCtx->ClearRenderTargetView(gPrivateTexRTV.Get(), clearColor);

    if (gDynamicPixelShader) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(gCtx->Map(gConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            ConstantBufferData cb{};
            cb.resolution[0] = (float)RENDER_W;
            cb.resolution[1] = (float)RENDER_H;
            cb.time = gTime;
            memcpy(mapped.pData, &cb, sizeof(cb));
            gCtx->Unmap(gConstantBuffer.Get(), 0);
        }
        gCtx->VSSetShader(gVertexShader.Get(), nullptr, 0);
        gCtx->PSSetShader(gDynamicPixelShader.Get(), nullptr, 0);
        gCtx->PSSetConstantBuffers(0, 1, gConstantBuffer.GetAddressOf());
        gCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        gCtx->Draw(3, 0);
    }
    
    gCtx->CopyResource(gSharedTex.Get(), gPrivateTex.Get());
    gFrameValue++;
    gCtx4->Signal(gSharedFence.Get(), gFrameValue);
    if (g_pManifestView) {
        InterlockedExchange64(reinterpret_cast<volatile LONGLONG*>(&g_pManifestView->frameValue), gFrameValue);
    }

    RECT rc; GetClientRect(hwnd, &rc);
    vp = { 0, 0, (float)(rc.right - rc.left), (float)(rc.bottom - rc.top), 0, 1 };
    gCtx->RSSetViewports(1, &vp);
    gCtx->OMSetRenderTargets(1, gWindowRTV.GetAddressOf(), nullptr);
    
    gCtx->VSSetShader(gVertexShader.Get(), nullptr, 0);
    gCtx->PSSetShader(gPassthroughPS.Get(), nullptr, 0);
    gCtx->PSSetShaderResources(0, 1, gSharedTexSRV.GetAddressOf());
    gCtx->PSSetSamplers(0, 1, gSamplerState.GetAddressOf());
    gCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gCtx->Draw(3, 0);
    
    ID3D11ShaderResourceView* nullSRV[] = { nullptr };
    gCtx->PSSetShaderResources(0, 1, nullSRV);

    gSwap->Present(1, 0);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    Log(L"Shader Producer D3D11 START.");
    const WCHAR szClassName[] = L"DirectPortShaderProducerD3D11Wnd";
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEXW) };
    wcex.lpfnWndProc = WndProc; wcex.hInstance = hInstance; wcex.lpszClassName = szClassName;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(DPD_ICON1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    wcex.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(DPD_ICON1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    RegisterClassExW(&wcex);

    DWORD pid = GetCurrentProcessId();
    gSharedTextureName = L"Global\\DirectPortTexture_" + std::to_wstring(pid);
    gSharedFenceName = L"Global\\DirectPortFence_" + std::to_wstring(pid);
    std::wstring title = L"Shader Producer (D3D11) (PID: " + std::to_wstring(pid) + L") - Press SPACE to load shader";
    HWND hwnd = CreateWindowExW(0, szClassName, title.c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, RENDER_W + 16, RENDER_H + 39, nullptr, nullptr, hInstance, nullptr);
    SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)wcex.hIcon);
    SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)wcex.hIconSm);

    if (FAILED(CreateDeviceAndResources(hwnd))) { MessageBoxW(NULL, L"Failed to create DirectX device and resources.", L"Startup Error", MB_OK); return 2; }
    if (FAILED(CreateSharedTexture())) { MessageBoxW(NULL, L"Failed to create shared texture.", L"Startup Error", MB_OK); return 2; }
    if (FAILED(CreateSharedFence())) { MessageBoxW(NULL, L"Failed to create shared fence.", L"Startup Error", MB_OK); return 2; }
    if (FAILED(CreateBroadcastManifest())) { MessageBoxW(NULL, L"Failed to create broadcast manifest.", L"Startup Error", MB_OK); return 2; }

    Log(L"Initialization successful. Entering render loop.");
    MSG msg{};
    while (msg.message != WM_QUIT) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
        } else {
            RenderFrame(hwnd);
        }
    }

    if (g_pManifestView) UnmapViewOfFile(g_pManifestView);
    if (g_hManifest) CloseHandle(g_hManifest);
    if (gSharedNTHandle) CloseHandle(gSharedNTHandle);
    if (gSharedFenceHandle) CloseHandle(gSharedFenceHandle);
    return (int)msg.wParam;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_SPACE) { LoadShader(hwnd); return 0; }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if (gSwap && wParam != SIZE_MINIMIZED) {
            OnResize(LOWORD(lParam), HIWORD(lParam));
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void OnResize(UINT width, UINT height) {
    if (!gSwap) return;
    gCtx->OMSetRenderTargets(0, 0, 0);
    gWindowRTV.Reset();
    HRESULT hr = gSwap->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    if (FAILED(hr)) { PostQuitMessage(1); return; }
    ComPtr<ID3D11Texture2D> backBuffer;
    hr = gSwap->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (SUCCEEDED(hr)) {
        gDev->CreateRenderTargetView(backBuffer.Get(), nullptr, &gWindowRTV);
    }
}

bool LoadShader(HWND hwnd) {
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd; ofn.lpstrFilter = L"Shader Files (*.cso, *.hlsl)\0*.cso;*.hlsl\0All Files\0*.*\0";
    ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrTitle = L"Open Pixel Shader";
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
        UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_SKIP_VALIDATION;
        HRESULT hr = D3DCompile(buffer.data(), buffer.size(), nullptr, nullptr, nullptr, "main", "ps_5_0", compileFlags, 0, &psBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) MessageBoxA(hwnd, (char*)errorBlob->GetBufferPointer(), "HLSL Compile Error", MB_OK);
            return false;
        }
    } else {
        HRESULT hr = D3DCreateBlob(buffer.size(), &psBlob);
        if (FAILED(hr)) return false;
        memcpy(psBlob->GetBufferPointer(), buffer.data(), buffer.size());
    }

    ComPtr<ID3D11PixelShader> ps;
    if (FAILED(gDev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps))) {
        MessageBoxW(hwnd, L"Failed to create Pixel Shader object from file.", L"D3D11 Error", MB_OK);
        return false;
    }
    gDynamicPixelShader = ps;

    std::wstring title = L"Shader Producer (D3D11) - Loaded: " + std::wstring(path);
    SetWindowTextW(hwnd, title.c_str());
    return true;
}

HRESULT CreateDeviceAndResources(HWND hwnd) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    #ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
    #endif
    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &sd, &gSwap, &gDev, nullptr, &gCtx);
    if (FAILED(hr)) { return hr; }
    hr = gDev.As(&gDev5); if (FAILED(hr)) { return hr; }
    hr = gCtx.As(&gCtx4); if (FAILED(hr)) { return hr; }
    
    RECT rc; GetClientRect(hwnd, &rc);
    OnResize(rc.right - rc.left, rc.bottom - rc.top);

    D3D11_TEXTURE2D_DESC privateDesc = {};
    privateDesc.Width = RENDER_W; privateDesc.Height = RENDER_H; privateDesc.MipLevels = 1; privateDesc.ArraySize = 1;
    privateDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; privateDesc.SampleDesc.Count = 1; privateDesc.Usage = D3D11_USAGE_DEFAULT;
    privateDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = gDev->CreateTexture2D(&privateDesc, nullptr, &gPrivateTex); if (FAILED(hr)) { return hr; }
    hr = gDev->CreateRenderTargetView(gPrivateTex.Get(), nullptr, &gPrivateTexRTV); if (FAILED(hr)) { return hr; }

    ComPtr<ID3DBlob> vsBlob, psBlob, err;
    D3DCompile(g_VertexShaderHLSL, strlen(g_VertexShaderHLSL), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &err);
    gDev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &gVertexShader);

    D3DCompile(g_PassthroughPixelShaderHLSL, strlen(g_PassthroughPixelShaderHLSL), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &err);
    gDev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &gPassthroughPS);

    D3D11_BUFFER_DESC cbDesc = {}; cbDesc.ByteWidth = sizeof(ConstantBufferData); cbDesc.Usage = D3D11_USAGE_DYNAMIC; cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    gDev->CreateBuffer(&cbDesc, nullptr, &gConstantBuffer);

    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    gDev->CreateSamplerState(&sampDesc, &gSamplerState);
    
    return S_OK;
}

HRESULT CreateSharedTexture() {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = RENDER_W; td.Height = RENDER_H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    HRESULT hr = gDev->CreateTexture2D(&td, nullptr, &gSharedTex);
    if (FAILED(hr)) { return hr; }
    
    hr = gDev->CreateShaderResourceView(gSharedTex.Get(), nullptr, &gSharedTexSRV);
    if (FAILED(hr)) { return hr; }

    ComPtr<IDXGIResource1> r1;
    hr = gSharedTex.As(&r1);
    if (FAILED(hr)) { return hr; }
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd, NULL)) {
        SECURITY_ATTRIBUTES sa{ sizeof(sa), sd, FALSE };
        hr = r1->CreateSharedHandle(&sa, GENERIC_ALL, gSharedTextureName.c_str(), &gSharedNTHandle);
        LocalFree(sd);
        if (FAILED(hr)) { return hr; }
    } else { return E_FAIL; }
    return S_OK;
}

HRESULT CreateSharedFence() {
    if (!gDev5) return E_NOINTERFACE;
    HRESULT hr = gDev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gSharedFence));
    if (FAILED(hr)) { return hr; }
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd, NULL)) {
        SECURITY_ATTRIBUTES sa{ sizeof(sa), sd, FALSE };
        hr = gSharedFence->CreateSharedHandle(&sa, GENERIC_ALL, gSharedFenceName.c_str(), &gSharedFenceHandle);
        LocalFree(sd);
        if (FAILED(hr)) { return hr; }
    } else { return E_FAIL; }
    return S_OK;
}

HRESULT CreateBroadcastManifest() {
    LUID adapterLuid = {};
    ComPtr<IDXGIDevice> dxgiDevice;
    if (SUCCEEDED(gDev.As(&dxgiDevice))) {
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC desc;
            if (SUCCEEDED(adapter->GetDesc(&desc))) { adapterLuid = desc.AdapterLuid; }
        }
    }
    std::wstring manifestName = L"DirectPort_Producer_Manifest_" + std::to_wstring(GetCurrentProcessId());
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd, NULL)) { return E_FAIL; }
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd, FALSE };
    g_hManifest = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(BroadcastManifest), manifestName.c_str());
    LocalFree(sd);
    if (g_hManifest == NULL) { return E_FAIL; }
    g_pManifestView = (BroadcastManifest*)MapViewOfFile(g_hManifest, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BroadcastManifest));
    if (g_pManifestView == nullptr) { CloseHandle(g_hManifest); g_hManifest = nullptr; return E_FAIL; }
    ZeroMemory(g_pManifestView, sizeof(BroadcastManifest));
    g_pManifestView->width = RENDER_W;
    g_pManifestView->height = RENDER_H;
    g_pManifestView->format = DXGI_FORMAT_B8G8R8A8_UNORM;
    g_pManifestView->adapterLuid = adapterLuid;
    wcscpy_s(g_pManifestView->textureName, _countof(g_pManifestView->textureName), gSharedTextureName.c_str());
    wcscpy_s(g_pManifestView->fenceName, _countof(g_pManifestView->fenceName), gSharedFenceName.c_str());
    return S_OK;
}