// --- DirectPortProducerD3D11.cpp ---

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

#include <d3d12.h>
#include "resource.h" 

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"user32.lib")
#pragma comment(lib,"d3dcompiler.lib")
#pragma comment(lib,"advapi32.lib")

using namespace Microsoft::WRL;

void Log(const std::wstring& msg) { WCHAR b[512]; wsprintfW(b, L"[PID:%lu][ProducerD3D11] %s\n", GetCurrentProcessId(), msg.c_str()); OutputDebugStringW(b); }
void LogHRESULT(const std::wstring& msg, HRESULT hr) { WCHAR b[512]; wsprintfW(b, L"[PID:%lu][ProducerD3D11] %s - HRESULT: 0x%08X\n", GetCurrentProcessId(), msg.c_str(), hr); OutputDebugStringW(b); }

static const UINT RENDER_W = 1280;
static const UINT RENDER_H = 1280;

struct BroadcastManifest {
    UINT64 frameValue;
    UINT width;
    UINT height;
    DXGI_FORMAT format;
    LUID adapterLuid;
    WCHAR textureName[256];
    WCHAR fenceName[256];
};
// Modified to match the HLSL shader's constant buffer layout
struct ConstantBuffer{ float bar_rect[4]; float resolution[2]; float time; float padding; };

static ComPtr<ID3D11Device>           gDev;
static ComPtr<ID3D11Device5>          gDev5;
static ComPtr<ID3D11DeviceContext>    gCtx;
static ComPtr<ID3D11DeviceContext4>   gCtx4;
static ComPtr<IDXGISwapChain>         gSwap;
static ComPtr<ID3D11RenderTargetView> gWindowRTV;
static ComPtr<ID3D11Buffer>           gConstantBuffer;
static ComPtr<ID3D11SamplerState>     gSamplerState;
static ComPtr<ID3D11VertexShader>     gVertexShaderGenerate;
static ComPtr<ID3D11PixelShader>      gPixelShaderGenerate;
static ComPtr<ID3D11VertexShader>     gVertexShaderPassthrough;
static ComPtr<ID3D11PixelShader>      gPixelShaderPassthrough;
static ComPtr<ID3D11Texture2D>        gPrivateTex;
static ComPtr<ID3D11RenderTargetView> gPrivateTexRTV;

static std::wstring                 gSharedTextureName, gSharedFenceName;
static ComPtr<ID3D11Texture2D>        gSharedTex;
static ComPtr<ID3D11ShaderResourceView> gSharedTexSRV;
static HANDLE                       gSharedNTHandle = nullptr;
static ComPtr<ID3D11Fence>            gSharedFence;
static HANDLE                       gSharedFenceHandle = nullptr;
static UINT64                       gFrameValue = 0;

static HANDLE                       g_hManifest = nullptr;
static BroadcastManifest*           g_pManifestView = nullptr;

// Time tracking for the new shader animation
static auto gStartTime = std::chrono::high_resolution_clock::now();

// This vertex shader is a simple passthrough that generates a full-screen triangle.
// It is perfectly suitable for the new pixel shader.
const char* g_GenerateVertexShaderHLSL = R"( struct VOut { float4 pos : SV_Position; }; VOut main(uint vid : SV_VertexID) { float2 uv = float2((vid << 1) & 2, vid & 2); VOut o; o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1); return o; } )";

// --- SHADER REPLACED WITH WarpSpeed.hlsl ---
const char* g_GeneratePixelShaderHLSL = R"(
cbuffer Constants : register(b0) {
    float4 bar_rect;           // unused (kept for CB layout parity)
    float2 resolution;         // (width, height)
    float  time;               // seconds
    float  pad;
}
// ==============================
//        T U N A B L E S
// ==============================
static const int   STAR_COUNT   = 120;
static const int   POINT_COUNT  = 180;
static const int   BACK_COUNT   = 96;
static const float STAR_BRIGHTNESS = 10.0f;
static const float ORIGIN_MIN   = 0.06f;
static const float ORIGIN_MAX   = 0.16f;
static const float BASE_SPEED   = 0.52f;
static const float TRAIL_BASE   = 0.14f;
static const float LINE_HALF_W  = 0.0018f;
static const float POINT_SIZE   = 0.0016f;
static const float POINT_SPEED  = 0.30f;
static const float BACK_SIZE    = 0.0024f;
static const float BACK_SPEED   = 0.0015f;
#define BACK_COLOR float3(1.0f, 1.0f, 1.0f)
static const float CA_START_FRAC    = 0.20f;
static const float CA_END_FRAC      = 0.750f;
static const float CA_CURVE         = 0.650f;
static const float CA_AMOUNT        = 0.040f;
static const float CA_CHANNEL_SCALE = 0.80f;
static const float CENTER_DIM_START_FRAC = 0.00f;
static const float CENTER_DIM_END_FRAC   = 0.40f;
static const float CENTER_DIM_STRENGTH   = 1.00f;
static const float PI   = 3.14159265f;
static const float RMAX = 1.50f;
// ==============================
//        I M P L E M E N T
// ==============================
float hash(float x) { return frac(sin(x) * 43758.5453123f); }
float ramp01(float x, float a, float b) { return saturate((x - a) / (b - a)); }
float3 rainbow_fast(float h) {
    float3 k = float3(0.0, 0.33, 0.66);
    float3 t = abs(frac(h + k) * 2.0 - 1.0);
    return saturate(1.0 - t * 0.9);
}
float4 main(float4 svpos : SV_Position) : SV_Target {
    float2 wh  = resolution;
    float2 ndc = svpos.xy / wh * float2( 2.0, -2.0) + float2(-1.0, 1.0);
    float r2   = dot(ndc, ndc);
    float r    = sqrt(r2) + 1e-6;
    float2 u   = ndc / r;
    float3 accum   = 0.0.xxx;
    float  starSum = 0.0;
    float caT = saturate((r - (CA_START_FRAC * RMAX)) / max(1e-4f, (CA_END_FRAC - CA_START_FRAC) * RMAX));
    caT = pow(caT, CA_CURVE);
    float caScale = CA_AMOUNT * caT;
    float edgeBright = lerp(0.92f, 1.15f, caT);
    float centerDimT   = 1.0f - saturate((r - (CENTER_DIM_START_FRAC * RMAX)) / max(1e-4f, (CENTER_DIM_END_FRAC - CENTER_DIM_START_FRAC) * RMAX));
    float centerDimFac = 1.0f - CENTER_DIM_STRENGTH * centerDimT;
    [loop]
    for (int i = 0; i < STAR_COUNT; ++i) {
        float si     = (float)i + 1.0f;
        float dRnd   = hash(si * 3.17f + 0.11f);
        float depth  = dRnd * dRnd;
        float speed  = lerp(0.06f, 1.00f, depth);
        float trailK = lerp(0.35f, 1.15f, depth);
        float thick  = lerp(0.65f, 1.35f, depth);
        float aRnd   = hash(si * 12.9898f + 0.4321f) * (2.0f * PI);
        float2 v     = float2(cos(aRnd), sin(aRnd));
        float rSpawn = lerp(ORIGIN_MIN, ORIGIN_MAX, hash(si * 7.23f + 0.77f));
        float phase  = frac(time * BASE_SPEED * speed + hash(si * 5.77f + 0.33f));
        float rHead  = rSpawn + phase * (RMAX - rSpawn);
        float rTail  = max(rSpawn, rHead - TRAIL_BASE * trailK);
        float dcos   = dot(u, v);
        float sinA   = sqrt(saturate(1.0f - dcos * dcos));
        float perp   = r * sinA;
        float halfW    = LINE_HALF_W * thick;
        float lineMask = saturate((halfW - abs(perp)) / halfW);
        float alongHead = ramp01(r, rHead - 0.01f, rHead);
        float alongTail = ramp01(r, rTail,        rTail + 0.01f);
        float alongWin  = alongTail * (1.0f - alongHead);
        float centerFade = ramp01(r, ORIGIN_MIN, ORIGIN_MAX + 0.10f);
        float fadeOut    = 1.0f - ramp01(r, RMAX * 0.94f, RMAX);
        float baseIntensity = lineMask * alongWin * centerFade * fadeOut;
        float hue      = frac((dcos * 0.5f + 0.5f) * 0.25f + r * 0.10f + si * 0.001f);
        float3 rbCol   = rainbow_fast(hue);
        float3 baseCol = lerp(1.0.xxx, rbCol, caT);
        float rShift = CA_CHANNEL_SCALE * caScale;
        float alongR = ramp01(r + rShift, rTail, rTail + 0.01f) * (1.0f - ramp01(r + rShift, rHead - 0.01f, rHead));
        float alongG = alongWin;
        float alongB = ramp01(r - rShift, rTail, rTail + 0.01f) * (1.0f - ramp01(r - rShift, rHead - 0.01f, rHead));
        float3 col = float3(baseCol.r * lineMask * alongR, baseCol.g * lineMask * alongG, baseCol.b * lineMask * alongB);
        accum   += (col * centerDimFac) * (baseIntensity * edgeBright);
        starSum += baseIntensity;
    }
    [loop]
    for (int j = 0; j < POINT_COUNT; ++j) {
        float sj    = (float)j + 101.0f;
        float depth = hash(sj * 9.19f + 0.21f); depth = depth * depth;
        float speed = lerp(0.003f, 0.08f, depth) * POINT_SPEED;
        float size  = lerp(0.8f,  1.8f,  depth) * POINT_SIZE;
        float ang0  = hash(sj * 2.71f + 0.37f) * (2.0f * PI);
        float2 dir  = float2(cos(ang0), sin(ang0));
        float r0    = lerp(0.05f, RMAX * 0.98f, hash(sj * 4.33f + 0.59f));
        float phase = frac((r0 / RMAX) + time * speed + hash(sj * 1.13f + 0.77f));
        float rP    = phase * RMAX;
        float2 p    = dir * rP;
        float d     = length(ndc - p);
        float pt    = 1.0f - saturate(d / size);
        pt          = pt * pt;
        accum   += float3(0.95, 0.96, 1.0) * 0.35f * pt;
        starSum += pt * 0.28f;
    }
    [loop]
    for (int k = 0; k < BACK_COUNT; ++k) {
        float sk    = (float)k + 555.0f;
        float angK  = hash(sk * 1.71f) * (2.0f * PI);
        float2 dirK = float2(cos(angK), sin(angK));
        float rK    = lerp(0.0f, RMAX, hash(sk * 3.99f));
        float phase = frac(rK + time * BACK_SPEED + hash(sk * 4.21f));
        float rBK   = phase * RMAX;
        float2 pB   = dirK * rBK;
        float dB    = length(ndc - pB);
        float pb    = 1.0f - saturate(dB / BACK_SIZE);
        pb          = pb * pb;
        accum   += BACK_COLOR * 0.45f * pb;
        starSum += pb * 0.10f;
    }
    float tone   = 1.0 / (1.0 + 0.9 * starSum);
    float3 color = saturate(accum * tone) * STAR_BRIGHTNESS;
    return float4(color, 1.0);
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


static HRESULT CreateDevice(HWND hwnd);
static HRESULT CreateSharedTexture();
static HRESULT CreateSharedFence();
static HRESULT CreateBroadcastManifest();
static void OnResize(UINT width, UINT height);
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
static void DrawMainScene(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv);

void RenderFrame(HWND hwnd) {
    if (!gWindowRTV) return; // Return if we are in a resizing state

    // PASS 1: Render the scene ONCE to the private texture
    DrawMainScene(gCtx.Get(), gPrivateTexRTV.Get());
    
    // Copy the result to the shared texture for consumers
    gCtx->CopyResource(gSharedTex.Get(), gPrivateTex.Get());
    gFrameValue++;
    gCtx4->Signal(gSharedFence.Get(), gFrameValue);

    if (g_pManifestView) {
        InterlockedExchange64(reinterpret_cast<volatile LONGLONG*>(&g_pManifestView->frameValue), gFrameValue);
    }
    
    // PASS 2: Blit the result to the window for a local preview
    RECT rc;
    GetClientRect(hwnd, &rc);
    D3D11_VIEWPORT vp = { 0, 0, (float)(rc.right - rc.left), (float)(rc.bottom - rc.top), 0, 1 };
    gCtx->RSSetViewports(1, &vp);

    gCtx->OMSetRenderTargets(1, gWindowRTV.GetAddressOf(), nullptr);

    // No need to clear here as we are blitting the full texture over the entire window
    // const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    // gCtx->ClearRenderTargetView(gWindowRTV.Get(), clearColor);

    gCtx->VSSetShader(gVertexShaderPassthrough.Get(), nullptr, 0);
    gCtx->PSSetShader(gPixelShaderPassthrough.Get(), nullptr, 0);
    gCtx->PSSetShaderResources(0, 1, gSharedTexSRV.GetAddressOf());
    gCtx->PSSetSamplers(0, 1, gSamplerState.GetAddressOf());
    gCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gCtx->Draw(3, 0);
    
    gSwap->Present(1, 0);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    Log(L"Producer START.");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    const WCHAR szClassName[] = L"DirectPortProducerD3D11Wnd";
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEXW) };
    wcex.lpfnWndProc   = WndProc;
    wcex.hInstance     = hInstance;
    wcex.lpszClassName = szClassName;
    wcex.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wcex.hIcon         = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(DPD_ICON1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    wcex.hIconSm       = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(DPD_ICON1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    RegisterClassExW(&wcex);

    DWORD pid = GetCurrentProcessId(); 
    gSharedTextureName = L"Global\\DirectPortTexture_" + std::to_wstring(pid);
    gSharedFenceName = L"Global\\DirectPortFence_" + std::to_wstring(pid);

    std::wstring title = L"DirectPort Producer (D3D11) (PID: " + std::to_wstring(pid) + L")";
    
    // Create the window initially hidden to prevent a white flash
    HWND hwnd = CreateWindowExW(0, szClassName, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, RENDER_W + 16, RENDER_H + 39, nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) { Log(L"CreateWindowExW FAILED."); return 1; }
    
    SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)wcex.hIcon);
    SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)wcex.hIconSm);

    if (FAILED(CreateDevice(hwnd))) { MessageBoxW(NULL, L"Failed to create DirectX device or compile shaders.", L"Startup Error", MB_OK); return 2; }
    
    // --- PRE-RENDER A BLACK FRAME ---
    // Ensure the first thing the user sees is a black screen.
    const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f }; // Black
    gCtx->ClearRenderTargetView(gWindowRTV.Get(), clearColor);
    gSwap->Present(0, 0); // Present the black frame immediately.
    // ---

    if (FAILED(CreateSharedTexture())) { MessageBoxW(NULL, L"Failed to create shared texture.", L"Startup Error", MB_OK); return 2; }
    if (FAILED(CreateSharedFence())) { MessageBoxW(NULL, L"Failed to create shared fence.", L"Startup Error", MB_OK); return 2; }
    if (FAILED(CreateBroadcastManifest())) { MessageBoxW(NULL, L"Failed to create broadcast manifest.", L"Startup Error", MB_OK); return 2; }
    
    // Now that everything is initialized and a black frame is ready, show the window.
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    gStartTime = std::chrono::high_resolution_clock::now(); // Initialize start time
    Log(L"Initialization successful. Entering render loop.");
    MSG msg{};
    while (msg.message != WM_QUIT) { if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); } else { RenderFrame(hwnd); } }
    
    if (g_pManifestView) UnmapViewOfFile(g_pManifestView);
    if (g_hManifest) CloseHandle(g_hManifest);
    if (gSharedNTHandle) CloseHandle(gSharedNTHandle);
    if (gSharedFenceHandle) CloseHandle(gSharedFenceHandle);
    CoUninitialize();
    Log(L"Application shutting down.");
    return (int)msg.wParam;
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_SIZE:
            if (gSwap && w != SIZE_MINIMIZED) {
                OnResize(LOWORD(l), HIWORD(l));
            }
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void OnResize(UINT width, UINT height) {
    if (!gSwap) return;

    gCtx->OMSetRenderTargets(0, 0, 0);
    gWindowRTV.Reset();

    HRESULT hr = gSwap->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    if (FAILED(hr)) {
        LogHRESULT(L"Swap chain resize failed", hr);
        PostQuitMessage(1);
        return;
    }

    ComPtr<ID3D11Texture2D> backBuffer;
    hr = gSwap->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (SUCCEEDED(hr)) {
        gDev->CreateRenderTargetView(backBuffer.Get(), nullptr, &gWindowRTV);
    }
}

HRESULT CreateDevice(HWND hwnd) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &sd, &gSwap, &gDev, nullptr, &gCtx);
    if (FAILED(hr)) { LogHRESULT(L"D3D11CreateDeviceAndSwapChain FAILED.", hr); return hr; }
    hr = gDev.As(&gDev5); if (FAILED(hr)) { LogHRESULT(L"Failed to query for ID3D11Device5.", hr); return hr; }
    hr = gCtx.As(&gCtx4); if (FAILED(hr)) { LogHRESULT(L"Failed to query for ID3D11DeviceContext4.", hr); return hr; }
    
    OnResize(RENDER_W, RENDER_H); // Initial setup of the RTV

    D3D11_TEXTURE2D_DESC privateDesc = {};
    privateDesc.Width=RENDER_W; privateDesc.Height=RENDER_H; privateDesc.MipLevels=1; privateDesc.ArraySize=1;
    privateDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM; privateDesc.SampleDesc.Count=1; privateDesc.Usage=D3D11_USAGE_DEFAULT;
    privateDesc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    hr = gDev->CreateTexture2D(&privateDesc,nullptr,&gPrivateTex);
    if (FAILED(hr)) { LogHRESULT(L"Failed to create private texture.", hr); return hr; }
    hr = gDev->CreateRenderTargetView(gPrivateTex.Get(), nullptr, &gPrivateTexRTV);
    if (FAILED(hr)) { LogHRESULT(L"Failed to create private RTV.", hr); return hr; }

    ComPtr<ID3DBlob> vsGen, psGen, vsPT, psPT, err;
    auto CompileShader = [&](const char* hlsl, const char* entry, const char* target, ComPtr<ID3DBlob>& blob) -> HRESULT {
        ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, entry, target, 0, 0, &blob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) {
                const char* errorMsg = (const char*)errorBlob->GetBufferPointer();
                Log(std::wstring(errorMsg, errorMsg + strlen(errorMsg)));
            }
             LogHRESULT(L"Shader compilation failed", hr);
            return hr;
        }
        return S_OK;
    };

    if (FAILED(CompileShader(g_GenerateVertexShaderHLSL, "main", "vs_5_0", vsGen))) return E_FAIL;
    if (FAILED(CompileShader(g_GeneratePixelShaderHLSL, "main", "ps_5_0", psGen))) return E_FAIL;
    if (FAILED(CompileShader(g_PassthroughVertexShaderHLSL, "main", "vs_5_0", vsPT))) return E_FAIL;
    if (FAILED(CompileShader(g_PassthroughPixelShaderHLSL, "main", "ps_5_0", psPT))) return E_FAIL;

    gDev->CreateVertexShader(vsGen->GetBufferPointer(), vsGen->GetBufferSize(), nullptr, &gVertexShaderGenerate);
    gDev->CreatePixelShader(psGen->GetBufferPointer(), psGen->GetBufferSize(), nullptr, &gPixelShaderGenerate);
    gDev->CreateVertexShader(vsPT->GetBufferPointer(), vsPT->GetBufferSize(), nullptr, &gVertexShaderPassthrough);
    gDev->CreatePixelShader(psPT->GetBufferPointer(), psPT->GetBufferSize(), nullptr, &gPixelShaderPassthrough);

    D3D11_BUFFER_DESC cbDesc = {}; cbDesc.ByteWidth = sizeof(ConstantBuffer); cbDesc.Usage = D3D11_USAGE_DYNAMIC; cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    gDev->CreateBuffer(&cbDesc, nullptr, &gConstantBuffer);
    D3D11_SAMPLER_DESC sampDesc = {}; sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP; sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP; sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER; sampDesc.MinLOD = 0; sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    gDev->CreateSamplerState(&sampDesc, &gSamplerState);
    return S_OK;
}

HRESULT CreateSharedTexture() {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = RENDER_W; td.Height = RENDER_H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    HRESULT hr = gDev->CreateTexture2D(&td, nullptr, &gSharedTex);
    if (FAILED(hr)) { LogHRESULT(L"CreateTexture2D for shared texture FAILED.", hr); return hr; }
    
    hr = gDev->CreateShaderResourceView(gSharedTex.Get(), nullptr, &gSharedTexSRV);
    if (FAILED(hr)) { LogHRESULT(L"CreateShaderResourceView for shared texture FAILED.", hr); return hr; }

    ComPtr<IDXGIResource1> r1;
    hr = gSharedTex.As(&r1);
    if (FAILED(hr)) { LogHRESULT(L"Failed to query IDXGIResource1 for texture.", hr); return hr; }
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd, NULL)){
        SECURITY_ATTRIBUTES sa{ sizeof(sa), sd, FALSE };
        hr = r1->CreateSharedHandle(&sa, GENERIC_ALL, gSharedTextureName.c_str(), &gSharedNTHandle);
        LocalFree(sd);
        if (FAILED(hr)) { LogHRESULT(L"CreateSharedHandle FAILED for texture.", hr); return hr; }
    } else { return E_FAIL; }
    return S_OK;
}

HRESULT CreateSharedFence() {
    if (!gDev5) return E_NOINTERFACE;
    HRESULT hr = gDev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gSharedFence));
    if (FAILED(hr)) { LogHRESULT(L"CreateFence FAILED.", hr); return hr; }
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd, NULL)) {
        SECURITY_ATTRIBUTES sa{ sizeof(sa), sd, FALSE };
        hr = gSharedFence->CreateSharedHandle(&sa, GENERIC_ALL, gSharedFenceName.c_str(), &gSharedFenceHandle);
        LocalFree(sd);
        if (FAILED(hr)) { LogHRESULT(L"CreateSharedHandle FAILED for fence.", hr); return hr; }
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
    if (g_hManifest == NULL) { LogHRESULT(L"CreateFileMappingW FAILED.", HRESULT_FROM_WIN32(GetLastError())); return E_FAIL; }
    g_pManifestView = (BroadcastManifest*)MapViewOfFile(g_hManifest, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BroadcastManifest));
    if (g_pManifestView == nullptr) { LogHRESULT(L"MapViewOfFile FAILED.", HRESULT_FROM_WIN32(GetLastError())); CloseHandle(g_hManifest); g_hManifest = nullptr; return E_FAIL; }
    
    ZeroMemory(g_pManifestView, sizeof(BroadcastManifest));
    g_pManifestView->width = RENDER_W;
    g_pManifestView->height = RENDER_H;
    g_pManifestView->format = DXGI_FORMAT_B8G8R8A8_UNORM;
    g_pManifestView->adapterLuid = adapterLuid;
    wcscpy_s(g_pManifestView->textureName, _countof(g_pManifestView->textureName), gSharedTextureName.c_str());
    wcscpy_s(g_pManifestView->fenceName, _countof(g_pManifestView->fenceName), gSharedFenceName.c_str());
    return S_OK;
}

void DrawMainScene(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv) {
    D3D11_VIEWPORT vp = { 0, 0, (float)RENDER_W, (float)RENDER_H, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(gConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        ConstantBuffer cb;
        cb.resolution[0] = RENDER_W; 
        cb.resolution[1] = RENDER_H;
        // Calculate elapsed time for the shader animation
        auto now = std::chrono::high_resolution_clock::now();
        cb.time = std::chrono::duration<float>(now - gStartTime).count();
        
        memcpy(mapped.pData, &cb, sizeof(cb));
        ctx->Unmap(gConstantBuffer.Get(), 0);
    }
    ctx->VSSetShader(gVertexShaderGenerate.Get(), nullptr, 0);
    ctx->PSSetShader(gPixelShaderGenerate.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, gConstantBuffer.GetAddressOf());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->Draw(3, 0);
}