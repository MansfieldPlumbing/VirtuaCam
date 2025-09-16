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
#include "resource.h"

#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"user32.lib")
#pragma comment(lib,"d3dcompiler.lib")
#pragma comment(lib,"advapi32.lib")

using namespace Microsoft::WRL;

void Log(const std::wstring& msg) { WCHAR b[512]; wsprintfW(b, L"[PID:%lu][T2B_ProducerD3D11] %s\n", GetCurrentProcessId(), msg.c_str()); OutputDebugStringW(b); }
void LogHRESULT(const std::wstring& msg, HRESULT hr) { WCHAR b[512]; wsprintfW(b, L"[PID:%lu][T2B_ProducerD3D11] %s - HRESULT: 0x%08X\n", GetCurrentProcessId(), msg.c_str(), hr); OutputDebugStringW(b); }

static const UINT RENDER_W = 1280;
static const UINT RENDER_H = 720;
static const DXGI_FORMAT RENDER_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;
static const UINT PIXEL_SIZE_BYTES = 4;

struct SharedBufferManifest {
    UINT64 frameValue;
    UINT64 bufferSize;
    LUID adapterLuid;
    UINT textureWidth;
    UINT textureHeight;
    DXGI_FORMAT textureFormat;
    WCHAR resourceName[256];
    WCHAR fenceName[256];
};

struct RenderConstants{ float bar_rect[4]; float resolution[2]; float time; float padding; };
struct ComputeConstants { UINT textureWidth; UINT padding[3]; };

static ComPtr<ID3D11Device>       gDev;
static ComPtr<ID3D11Device5>      gDev5;
static ComPtr<ID3D11DeviceContext>    gCtx;
static ComPtr<ID3D11DeviceContext4>   gCtx4;
static ComPtr<IDXGISwapChain>      gSwap;
static ComPtr<ID3D11RenderTargetView> gWindowRTV;
static ComPtr<ID3D11Buffer>       gRenderConstantBuffer;
static ComPtr<ID3D11Buffer>       gComputeConstantBuffer;
static ComPtr<ID3D11SamplerState>     gSamplerState;
static ComPtr<ID3D11VertexShader>     gVertexShaderGenerate;
static ComPtr<ID3D11PixelShader>      gPixelShaderGenerate;
static ComPtr<ID3D11VertexShader>     gVertexShaderPassthrough;
static ComPtr<ID3D11PixelShader>      gPixelShaderPassthrough;
static ComPtr<ID3D11ComputeShader>    gComputeShaderT2B;

static ComPtr<ID3D11Texture2D>        gPrivateTex;
static ComPtr<ID3D11RenderTargetView> gPrivateTexRTV;
static ComPtr<ID3D11ShaderResourceView> gPrivateTexSRV;

static std::wstring           gSharedBufferName, gSharedFenceName;
static ComPtr<ID3D11Buffer>       gSharedBuffer;
static ComPtr<ID3D11UnorderedAccessView> gSharedBufferUAV;
static HANDLE               gSharedBufferNTHandle = nullptr;
static ComPtr<ID3D11Fence>        gSharedBufferFence;
static HANDLE               gSharedFenceHandle = nullptr;
static UINT64               gBufferFrameValue = 0;

static HANDLE               g_hBufManifest = nullptr;
static SharedBufferManifest* g_pBufManifestView = nullptr;

static auto gStartTime = std::chrono::high_resolution_clock::now();

const char* g_GenerateVertexShaderHLSL = R"( struct VOut { float4 pos : SV_Position; }; VOut main(uint vid : SV_VertexID) { float2 uv = float2((vid << 1) & 2, vid & 2); VOut o; o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1); return o; } )";
const char* g_GeneratePixelShaderHLSL = R"(
cbuffer Constants : register(b0) {
    float4 bar_rect; float2 resolution; float  time; float  pad;
}
static const int    STAR_COUNT    = 120; static const int    POINT_COUNT  = 180; static const int    BACK_COUNT     = 96;
static const float STAR_BRIGHTNESS = 10.0f; static const float ORIGIN_MIN    = 0.06f; static const float ORIGIN_MAX    = 0.16f;
static const float BASE_SPEED    = 0.52f; static const float TRAIL_BASE    = 0.14f; static const float LINE_HALF_W  = 0.0018f;
static const float POINT_SIZE    = 0.0016f; static const float POINT_SPEED  = 0.30f; static const float BACK_SIZE     = 0.0024f;
static const float BACK_SPEED    = 0.0015f;
#define BACK_COLOR float3(1.0f, 1.0f, 1.0f)
static const float CA_START_FRAC     = 0.20f; static const float CA_END_FRAC      = 0.750f; static const float CA_CURVE           = 0.650f;
static const float CA_AMOUNT          = 0.040f; static const float CA_CHANNEL_SCALE = 0.80f; static const float CENTER_DIM_START_FRAC = 0.00f;
static const float CENTER_DIM_END_FRAC     = 0.40f; static const float CENTER_DIM_STRENGTH     = 1.00f; static const float PI    = 3.14159265f;
static const float RMAX = 1.50f;
float hash(float x) { return frac(sin(x) * 43758.5453123f); }
float ramp01(float x, float a, float b) { return saturate((x - a) / (b - a)); }
float4 main(float4 svpos : SV_Position) : SV_Target {
    float2 wh  = resolution; float2 ndc = svpos.xy / wh * float2( 2.0, -2.0) + float2(-1.0, 1.0);
    float r    = length(ndc) + 1e-6; float2 u = ndc / r; float3 accum = 0.0.xxx; float starSum = 0.0;
    float caT = pow(saturate((r - (CA_START_FRAC * RMAX)) / max(1e-4f, (CA_END_FRAC - CA_START_FRAC) * RMAX)), CA_CURVE);
    float caScale = CA_AMOUNT * caT; float edgeBright = lerp(0.92f, 1.15f, caT);
    float centerDimFac = 1.0f - CENTER_DIM_STRENGTH * (1.0f - saturate((r - (CENTER_DIM_START_FRAC * RMAX)) / max(1e-4f, (CENTER_DIM_END_FRAC - CA_START_FRAC) * RMAX)));
    [loop] for (int i = 0; i < STAR_COUNT; ++i) {
        float si = (float)i + 1.0f; float depth  = hash(si*3.17f+0.11f); depth*=depth;
        float speed = lerp(0.06f, 1.00f, depth); float trailK = lerp(0.35f, 1.15f, depth); float thick = lerp(0.65f, 1.35f, depth);
        float2 v = float2(cos(hash(si*12.9898f+0.4321f)*2*PI), sin(hash(si*12.9898f+0.4321f)*2*PI));
        float rSpawn = lerp(ORIGIN_MIN, ORIGIN_MAX, hash(si*7.23f+0.77f));
        float rHead  = rSpawn + frac(time*BASE_SPEED*speed + hash(si*5.77f+0.33f)) * (RMAX - rSpawn);
        float rTail  = max(rSpawn, rHead - TRAIL_BASE * trailK); float dcos = dot(u, v);
        float perp = r * sqrt(saturate(1-dcos*dcos)); float lineMask = saturate((LINE_HALF_W*thick - abs(perp)) / (LINE_HALF_W*thick));
        float alongWin = ramp01(r, rTail, rTail+0.01f) * (1-ramp01(r, rHead-0.01f, rHead));
        float baseIntensity = lineMask * alongWin * ramp01(r, ORIGIN_MIN, ORIGIN_MAX+0.1f) * (1-ramp01(r, RMAX*0.94f, RMAX));
        float3 baseCol = 1.0.xxx; float rShift = CA_CHANNEL_SCALE * caScale;
        float3 col = float3(baseCol.r*lineMask*ramp01(r+rShift, rTail, rTail+0.01f)*(1-ramp01(r+rShift, rHead-0.01f, rHead)), baseCol.g*lineMask*alongWin, baseCol.b*lineMask*ramp01(r-rShift, rTail, rTail+0.01f)*(1-ramp01(r-rShift, rHead-0.01f, rHead)));
        accum += (col * centerDimFac) * (baseIntensity * edgeBright); starSum += baseIntensity;
    }
    [loop] for (int j = 0; j < POINT_COUNT; ++j) {
        float sj = (float)j+101.f; float depth=hash(sj*9.19f+0.21f); depth*=depth;
        float2 dir = float2(cos(hash(sj*2.71f+0.37f)*2*PI), sin(hash(sj*2.71f+0.37f)*2*PI));
        float r0 = lerp(0.05f, RMAX*0.98f, hash(sj*4.33f+0.59f));
        float2 p = dir * (frac((r0/RMAX) + time*lerp(0.003f,0.08f,depth)*POINT_SPEED + hash(sj*1.13f+0.77f)) * RMAX);
        float pt = 1-saturate(length(ndc-p)/(lerp(0.8f,1.8f,depth)*POINT_SIZE)); pt*=pt;
        accum += float3(0.95,0.96,1.0)*0.35f*pt; starSum += pt*0.28f;
    }
    [loop] for (int k = 0; k < BACK_COUNT; ++k) {
        float sk = (float)k+555.f; float2 dirK = float2(cos(hash(sk*1.71f)*2*PI), sin(hash(sk*1.71f)*2*PI));
        float rK = lerp(0, RMAX, hash(sk*3.99f)); float2 pB = dirK * (frac(rK+time*BACK_SPEED+hash(sk*4.21f))*RMAX);
        float pb = 1-saturate(length(ndc-pB)/BACK_SIZE); pb*=pb;
        accum += BACK_COLOR*0.45f*pb; starSum += pb*0.1f;
    }
    return float4(saturate(accum * (1.0/(1.0+0.9*starSum))) * STAR_BRIGHTNESS, 1.0);
}
)";
const char* g_PassthroughVertexShaderHLSL = R"(
    struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD; };
    VOut main(uint vid : SV_VertexID) {
        float2 uv = float2((vid << 1) & 2, vid & 2); VOut o;
        o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1); o.uv = uv;
        return o;
    })";
const char* g_PassthroughPixelShaderHLSL = R"(
    Texture2D g_texture : register(t0); SamplerState g_sampler : register(s0);
    struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
    float4 main(PSInput input) : SV_TARGET { return g_texture.Sample(g_sampler, input.uv); })";
const char* g_computeShaderT2B_HLSL = R"(
Texture2D<float4> g_inputTexture : register(t0);
RWStructuredBuffer<float4> g_outputBuffer : register(u0);

cbuffer ComputeConstants : register(b0)
{
    uint textureWidth;
};

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= textureWidth) return;
    uint index = DTid.y * textureWidth + DTid.x;
    g_outputBuffer[index] = g_inputTexture[DTid.xy];
}
)";

static HRESULT CreateDeviceAndResources(HWND hwnd);
static HRESULT CreateSharedResources();
static void OnResize(UINT width, UINT height);
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
static void DrawMainScene(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv);

void RenderFrame(HWND hwnd) {
    if (!gWindowRTV) return;

    DrawMainScene(gCtx.Get(), gPrivateTexRTV.Get());

    gCtx->CSSetShader(gComputeShaderT2B.Get(), nullptr, 0);
    gCtx->CSSetConstantBuffers(0, 1, gComputeConstantBuffer.GetAddressOf());
    gCtx->CSSetShaderResources(0, 1, gPrivateTexSRV.GetAddressOf());
    gCtx->CSSetUnorderedAccessViews(0, 1, gSharedBufferUAV.GetAddressOf(), nullptr);

    gCtx->Dispatch( (UINT)ceilf(RENDER_W / 8.0f), (UINT)ceilf(RENDER_H / 8.0f), 1);
    
    ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
    gCtx->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
    ID3D11ShaderResourceView* nullSRV[] = { nullptr };
    gCtx->CSSetShaderResources(0, 1, nullSRV);
    
    gBufferFrameValue++;
    gCtx4->Signal(gSharedBufferFence.Get(), gBufferFrameValue);

    if (g_pBufManifestView) {
        InterlockedExchange64(reinterpret_cast<volatile LONGLONG*>(&g_pBufManifestView->frameValue), gBufferFrameValue);
    }
    
    RECT rc; GetClientRect(hwnd, &rc);
    D3D11_VIEWPORT vp = { 0, 0, (float)(rc.right - rc.left), (float)(rc.bottom - rc.top), 0, 1 };
    gCtx->RSSetViewports(1, &vp);
    gCtx->OMSetRenderTargets(1, gWindowRTV.GetAddressOf(), nullptr);
    gCtx->VSSetShader(gVertexShaderPassthrough.Get(), nullptr, 0);
    gCtx->PSSetShader(gPixelShaderPassthrough.Get(), nullptr, 0);
    gCtx->PSSetShaderResources(0, 1, gPrivateTexSRV.GetAddressOf());
    gCtx->PSSetSamplers(0, 1, gSamplerState.GetAddressOf());
    gCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gCtx->Draw(3, 0);
    
    gSwap->Present(1, 0);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    Log(L"Texture-to-Buffer Producer START.");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    const WCHAR szClassName[] = L"DirectPortT2BProducerD3D11Wnd";
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEXW) };
    wcex.lpfnWndProc     = WndProc;
    wcex.hInstance       = hInstance;
    wcex.lpszClassName = szClassName;
    wcex.hCursor         = LoadCursor(nullptr, IDC_ARROW);
    wcex.hIcon           = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(DPD_ICON1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    wcex.hIconSm         = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(DPD_ICON1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    RegisterClassExW(&wcex);

    DWORD pid = GetCurrentProcessId(); 
    gSharedBufferName = L"Global\\DirectPortT2B_Buffer_" + std::to_wstring(pid);
    gSharedFenceName = L"Global\\DirectPortT2B_Fence_" + std::to_wstring(pid);
    std::wstring title = L"Texture-to-Buffer Producer (D3D11) (PID: " + std::to_wstring(pid) + L")";
    
    HWND hwnd = CreateWindowExW(0, szClassName, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, RENDER_W + 16, RENDER_H + 39, nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) { Log(L"CreateWindowExW FAILED."); return 1; }
    
    SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)wcex.hIcon);
    SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)wcex.hIconSm);

    if (FAILED(CreateDeviceAndResources(hwnd))) { MessageBoxW(NULL, L"Failed to create DirectX device or compile shaders.", L"Startup Error", MB_OK); return 2; }
    if (FAILED(CreateSharedResources())) { MessageBoxW(NULL, L"Failed to create shared buffer or manifest.", L"Startup Error", MB_OK); return 2; }
    
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    gStartTime = std::chrono::high_resolution_clock::now();
    Log(L"Initialization successful. Entering render loop.");
    MSG msg{};
    while (msg.message != WM_QUIT) { if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); } else { RenderFrame(hwnd); } }
    
    if (g_pBufManifestView) UnmapViewOfFile(g_pBufManifestView);
    if (g_hBufManifest) CloseHandle(g_hBufManifest);
    if (gSharedBufferNTHandle) CloseHandle(gSharedBufferNTHandle);
    if (gSharedFenceHandle) CloseHandle(gSharedFenceHandle);
    CoUninitialize();
    Log(L"Application shutting down.");
    return (int)msg.wParam;
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_DESTROY: PostQuitMessage(0); return 0;
        case WM_SIZE: if (gSwap && w != SIZE_MINIMIZED) { OnResize(LOWORD(l), HIWORD(l)); } return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void OnResize(UINT width, UINT height) {
    if (!gSwap) return;
    gCtx->OMSetRenderTargets(0, 0, 0);
    gWindowRTV.Reset();
    HRESULT hr = gSwap->ResizeBuffers(2, width, height, RENDER_FORMAT, 0);
    if (FAILED(hr)) { PostQuitMessage(1); return; }
    ComPtr<ID3D11Texture2D> backBuffer;
    hr = gSwap->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (SUCCEEDED(hr)) { gDev->CreateRenderTargetView(backBuffer.Get(), nullptr, &gWindowRTV); }
}

HRESULT CreateDeviceAndResources(HWND hwnd) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    // flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount = 2; sd.BufferDesc.Format = RENDER_FORMAT; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &sd, &gSwap, &gDev, nullptr, &gCtx);
    if (FAILED(hr)) { return hr; }
    hr = gDev.As(&gDev5); if (FAILED(hr)) return hr;
    hr = gCtx.As(&gCtx4); if (FAILED(hr)) return hr;
    
    OnResize(RENDER_W, RENDER_H);

    D3D11_TEXTURE2D_DESC privateDesc = {};
    privateDesc.Width=RENDER_W; privateDesc.Height=RENDER_H; privateDesc.MipLevels=1; privateDesc.ArraySize=1;
    privateDesc.Format=RENDER_FORMAT; privateDesc.SampleDesc.Count=1; privateDesc.Usage=D3D11_USAGE_DEFAULT;
    privateDesc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    gDev->CreateTexture2D(&privateDesc,nullptr,&gPrivateTex);
    gDev->CreateRenderTargetView(gPrivateTex.Get(), nullptr, &gPrivateTexRTV);
    gDev->CreateShaderResourceView(gPrivateTex.Get(), nullptr, &gPrivateTexSRV);
    
    ComPtr<ID3DBlob> vsGen, psGen, vsPT, psPT, csT2B;
    auto CompileShader = [&](const char* hlsl, const char* entry, const char* target, ComPtr<ID3DBlob>& blob) -> HRESULT {
        ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, entry, target, 0, 0, &blob, &errorBlob);
        if (FAILED(hr) && errorBlob) { 
            const char* errorMsg = (const char*)errorBlob->GetBufferPointer();
            Log(std::wstring(errorMsg, errorMsg + strlen(errorMsg)));
        }
        return hr;
    };
    if (FAILED(CompileShader(g_GenerateVertexShaderHLSL, "main", "vs_5_0", vsGen))) return E_FAIL;
    if (FAILED(CompileShader(g_GeneratePixelShaderHLSL, "main", "ps_5_0", psGen))) return E_FAIL;
    if (FAILED(CompileShader(g_PassthroughVertexShaderHLSL, "main", "vs_5_0", vsPT))) return E_FAIL;
    if (FAILED(CompileShader(g_PassthroughPixelShaderHLSL, "main", "ps_5_0", psPT))) return E_FAIL;
    if (FAILED(CompileShader(g_computeShaderT2B_HLSL, "main", "cs_5_0", csT2B))) return E_FAIL;

    gDev->CreateVertexShader(vsGen->GetBufferPointer(), vsGen->GetBufferSize(), nullptr, &gVertexShaderGenerate);
    gDev->CreatePixelShader(psGen->GetBufferPointer(), psGen->GetBufferSize(), nullptr, &gPixelShaderGenerate);
    gDev->CreateVertexShader(vsPT->GetBufferPointer(), vsPT->GetBufferSize(), nullptr, &gVertexShaderPassthrough);
    gDev->CreatePixelShader(psPT->GetBufferPointer(), psPT->GetBufferSize(), nullptr, &gPixelShaderPassthrough);
    gDev->CreateComputeShader(csT2B->GetBufferPointer(), csT2B->GetBufferSize(), nullptr, &gComputeShaderT2B);
    
    D3D11_BUFFER_DESC cbDesc = {}; cbDesc.ByteWidth = sizeof(RenderConstants); cbDesc.Usage = D3D11_USAGE_DYNAMIC; cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    gDev->CreateBuffer(&cbDesc, nullptr, &gRenderConstantBuffer);

    cbDesc.ByteWidth = sizeof(ComputeConstants);
    gDev->CreateBuffer(&cbDesc, nullptr, &gComputeConstantBuffer);
    D3D11_MAPPED_SUBRESOURCE mapped;
    gCtx->Map(gComputeConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    ComputeConstants cc = { RENDER_W };
    memcpy(mapped.pData, &cc, sizeof(cc));
    gCtx->Unmap(gComputeConstantBuffer.Get(), 0);

    D3D11_SAMPLER_DESC sampDesc = {}; sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP; sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP; sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER; sampDesc.MinLOD = 0; sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    gDev->CreateSamplerState(&sampDesc, &gSamplerState);
    return S_OK;
}

HRESULT CreateSharedResources() {
    const UINT bufferSize = RENDER_W * RENDER_H * PIXEL_SIZE_BYTES;
    D3D11_BUFFER_DESC bufDesc = {};
    bufDesc.ByteWidth = bufferSize;
    bufDesc.Usage = D3D11_USAGE_DEFAULT;
    bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS; // FIX: Removed D3D11_BIND_SHADER_RESOURCE
    bufDesc.CPUAccessFlags = 0;
    bufDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bufDesc.StructureByteStride = sizeof(float) * 4;
    HRESULT hr = gDev->CreateBuffer(&bufDesc, nullptr, &gSharedBuffer);
    if (FAILED(hr)) { LogHRESULT(L"CreateBuffer for shared buffer FAILED.", hr); return hr; }
    
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = RENDER_W * RENDER_H;
    hr = gDev->CreateUnorderedAccessView(gSharedBuffer.Get(), &uavDesc, &gSharedBufferUAV);
    if (FAILED(hr)) { LogHRESULT(L"CreateUnorderedAccessView for shared buffer FAILED.", hr); return hr; }
    
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd, NULL)) {
        LogHRESULT(L"ConvertStringSecurityDescriptorToSecurityDescriptorW FAILED.", HRESULT_FROM_WIN32(GetLastError()));
        return E_FAIL;
    }
    SECURITY_ATTRIBUTES sa{ sizeof(sa), sd, FALSE };
    
    ComPtr<IDXGIResource1> r1;
    hr = gSharedBuffer.As(&r1);
    if (FAILED(hr)) { LocalFree(sd); LogHRESULT(L"Failed to query IDXGIResource1 for buffer.", hr); return hr; }
    hr = r1->CreateSharedHandle(&sa, GENERIC_ALL, gSharedBufferName.c_str(), &gSharedBufferNTHandle);
    if (FAILED(hr)) { LocalFree(sd); LogHRESULT(L"CreateSharedHandle FAILED for buffer.", hr); return hr; }

    hr = gDev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gSharedBufferFence));
    if (FAILED(hr)) { LocalFree(sd); LogHRESULT(L"CreateFence FAILED.", hr); return hr; }
    hr = gSharedBufferFence->CreateSharedHandle(&sa, GENERIC_ALL, gSharedFenceName.c_str(), &gSharedFenceHandle);
    if (FAILED(hr)) { LocalFree(sd); LogHRESULT(L"CreateSharedHandle FAILED for fence.", hr); return hr; }

    ComPtr<IDXGIDevice> dxgiDevice; 
    hr = gDev.As(&dxgiDevice);
    if (FAILED(hr)) { LocalFree(sd); LogHRESULT(L"Failed to query IDXGIDevice.", hr); return hr; }
    ComPtr<IDXGIAdapter> adapter; 
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) { LocalFree(sd); LogHRESULT(L"Failed to get DXGI adapter.", hr); return hr; }
    DXGI_ADAPTER_DESC desc; 
    hr = adapter->GetDesc(&desc);
    if (FAILED(hr)) { LocalFree(sd); LogHRESULT(L"Failed to get adapter description.", hr); return hr; }
    
    std::wstring manifestName = L"Global\\DirectPort_T2B_Producer_Manifest_" + std::to_wstring(GetCurrentProcessId());
    g_hBufManifest = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(SharedBufferManifest), manifestName.c_str());
    if (g_hBufManifest == NULL) {
        LocalFree(sd);
        LogHRESULT(L"CreateFileMappingW FAILED.", HRESULT_FROM_WIN32(GetLastError()));
        return E_FAIL;
    }
    g_pBufManifestView = (SharedBufferManifest*)MapViewOfFile(g_hBufManifest, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedBufferManifest));
    if (g_pBufManifestView == nullptr) {
        CloseHandle(g_hBufManifest);
        LocalFree(sd);
        LogHRESULT(L"MapViewOfFile FAILED.", HRESULT_FROM_WIN32(GetLastError()));
        return E_FAIL;
    }
    
    ZeroMemory(g_pBufManifestView, sizeof(SharedBufferManifest));
    g_pBufManifestView->bufferSize = bufferSize;
    g_pBufManifestView->adapterLuid = desc.AdapterLuid;
    g_pBufManifestView->textureWidth = RENDER_W;
    g_pBufManifestView->textureHeight = RENDER_H;
    g_pBufManifestView->textureFormat = RENDER_FORMAT;
    wcscpy_s(g_pBufManifestView->resourceName, _countof(g_pBufManifestView->resourceName), gSharedBufferName.c_str());
    wcscpy_s(g_pBufManifestView->fenceName, _countof(g_pBufManifestView->fenceName), gSharedFenceName.c_str());
    LocalFree(sd);
    return S_OK;
}

void DrawMainScene(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv) {
    D3D11_VIEWPORT vp = { 0, 0, (float)RENDER_W, (float)RENDER_H, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(gRenderConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        RenderConstants cb;
        cb.resolution[0] = RENDER_W; cb.resolution[1] = RENDER_H;
        cb.time = std::chrono::duration<float>(std::chrono::high_resolution_clock::now() - gStartTime).count();
        memcpy(mapped.pData, &cb, sizeof(cb));
        ctx->Unmap(gRenderConstantBuffer.Get(), 0);
    }
    ctx->VSSetShader(gVertexShaderGenerate.Get(), nullptr, 0);
    ctx->PSSetShader(gPixelShaderGenerate.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, gRenderConstantBuffer.GetAddressOf());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->Draw(3, 0);
}
