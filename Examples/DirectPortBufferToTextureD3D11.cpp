#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl.h>
#include <sddl.h>
#include <string>
#include <iostream>
#include <vector>
#include <d3d12.h>
#include <shellapi.h>
#include <tlhelp32.h> 
#include <chrono>   

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "d3d12.lib")

using namespace Microsoft::WRL;

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

struct PSConstants {
    UINT resolution[2];
    float padding[2];
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
    StructuredBuffer<float4> g_InputBuffer : register(t0);
    cbuffer Constants : register(b0) {
        uint2 resolution;
    };
    float4 PSMain(float4 pos : SV_Position) : SV_TARGET {
        uint index = (uint)pos.y * resolution.x + (uint)pos.x;
        return g_InputBuffer[index];
    }
)";

HANDLE GetHandleFromName(const WCHAR* name) {
    ComPtr<ID3D12Device> d3d12Device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12Device)))) return NULL;
    HANDLE handle = nullptr;
    d3d12Device->OpenSharedHandleByName(name, GENERIC_ALL, &handle);
    return handle;
}

HWND g_hwnd = nullptr;
ComPtr<IDXGISwapChain1> g_swapChain;
ComPtr<ID3D11RenderTargetView> g_renderTargetView;
ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11Device1> g_device1;
ComPtr<ID3D11Device5> g_device5;
ComPtr<ID3D11DeviceContext> g_context;
ComPtr<ID3D11DeviceContext4> g_context4;
ComPtr<ID3D11VertexShader> g_vertexShader;
ComPtr<ID3D11PixelShader> g_pixelShader;
ComPtr<ID3D11Buffer> g_psConstantBuffer;

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void CreateSwapchainResources();
void UpdateWindowTitle();
void DisconnectFromProducer();
void FindAndConnectToProducer();

static bool g_isConnected = false;
static DWORD g_producerPid = 0;
static SharedBufferManifest g_producerManifest = {};
static ComPtr<ID3D11Buffer> g_sharedBufferIn;
static ComPtr<ID3D11ShaderResourceView> g_sharedBufferSRV;
static ComPtr<ID3D11Fence> g_sharedFenceIn;
static UINT64 g_lastSeenFrame = 0;

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int nCmdShow) {
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance, NULL, NULL, NULL, NULL, L"BufferToTextureD3D11", NULL };
    RegisterClassExW(&wc);
    g_hwnd = CreateWindowW(wc.lpszClassName, L"Buffer To Texture D3D11", WS_OVERLAPPEDWINDOW, 100, 100, 1280, 720, NULL, NULL, hInstance, NULL);

    UpdateWindowTitle();

    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g_device, nullptr, &g_context);
    g_device.As(&g_device1);
    g_device.As(&g_device5);
    g_context.As(&g_context4);

    ComPtr<IDXGIDevice> dxgiDevice; g_device.As(&dxgiDevice);
    ComPtr<IDXGIAdapter> dxgiAdapter; dxgiDevice->GetAdapter(&dxgiAdapter);
    ComPtr<IDXGIFactory2> dxgiFactory; dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory));

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = 2;
    swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;
    dxgiFactory->CreateSwapChainForHwnd(g_device.Get(), g_hwnd, &swapChainDesc, nullptr, nullptr, &g_swapChain);
    CreateSwapchainResources();

    ComPtr<ID3DBlob> vsBlob, psBlob, errors;
    D3DCompile(g_vertexShaderHLSL, strlen(g_vertexShaderHLSL), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsBlob, &errors);
    D3DCompile(g_pixelShaderHLSL, strlen(g_pixelShaderHLSL), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psBlob, &errors);

    g_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_vertexShader);
    g_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_pixelShader);

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(PSConstants);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_device->CreateBuffer(&cbDesc, nullptr, &g_psConstantBuffer);

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        FindAndConnectToProducer();

        if (g_isConnected) {
            UINT64 latestFrameValueInManifest = g_producerManifest.frameValue;
            if (latestFrameValueInManifest > g_lastSeenFrame) {
                 g_context4->Wait(g_sharedFenceIn.Get(), latestFrameValueInManifest);
                 g_lastSeenFrame = latestFrameValueInManifest;
                
                g_context->OMSetRenderTargets(1, g_renderTargetView.GetAddressOf(), nullptr);
                RECT rc; GetClientRect(g_hwnd, &rc);
                D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)rc.right, (float)rc.bottom, 0.0f, 1.0f };
                g_context->RSSetViewports(1, &vp);
                
                D3D11_MAPPED_SUBRESOURCE mapped;
                if (SUCCEEDED(g_context->Map(g_psConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                    PSConstants consts = { g_producerManifest.textureWidth, g_producerManifest.textureHeight };
                    memcpy(mapped.pData, &consts, sizeof(consts));
                    g_context->Unmap(g_psConstantBuffer.Get(), 0);
                }

                g_context->VSSetShader(g_vertexShader.Get(), nullptr, 0);
                g_context->PSSetShader(g_pixelShader.Get(), nullptr, 0);
                ID3D11ShaderResourceView* srvs[] = { g_sharedBufferSRV.Get() };
                g_context->PSSetShaderResources(0, 1, srvs);
                ID3D11Buffer* cbs[] = { g_psConstantBuffer.Get() };
                g_context->PSSetConstantBuffers(0, 1, cbs);
                
                g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                g_context->Draw(3, 0);
            }
        } else {
             const float clearColor[] = { 0.1f, 0.0f, 0.1f, 1.0f };
             g_context->ClearRenderTargetView(g_renderTargetView.Get(), clearColor);
        }

        g_swapChain->Present(1, 0);
    }

    DisconnectFromProducer();
    return 0;
}

void CreateSwapchainResources() {
    g_renderTargetView.Reset();
    ComPtr<ID3D11Texture2D> pBackBuffer;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_device->CreateRenderTargetView(pBackBuffer.Get(), NULL, &g_renderTargetView);
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
            std::wstring manifestName = L"DirectPort_T2B_Producer_Manifest_" + std::to_wstring(pe32.th32ProcessID);
            HANDLE hManifest = OpenFileMappingW(FILE_MAP_READ, FALSE, manifestName.c_str());
            if (hManifest) {
                SharedBufferManifest* manifestView = (SharedBufferManifest*)MapViewOfFile(hManifest, FILE_MAP_READ, 0, 0, sizeof(SharedBufferManifest));
                if (manifestView) {
                    HANDLE hFence = GetHandleFromName(manifestView->fenceName);
                    HANDLE hBuffer = GetHandleFromName(manifestView->resourceName);

                    ComPtr<ID3D11Fence> tempFence;
                    ComPtr<ID3D11Buffer> tempBuffer;
                    
                    if (hFence && hBuffer && 
                        SUCCEEDED(g_device5->OpenSharedFence(hFence, IID_PPV_ARGS(&tempFence))) && 
                        SUCCEEDED(g_device1->OpenSharedResource1(hBuffer, IID_PPV_ARGS(&tempBuffer)))) {
                        
                        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
                        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
                        srvDesc.Buffer.FirstElement = 0;
                        srvDesc.Buffer.NumElements = (UINT)(manifestView->bufferSize / (sizeof(float) * 4));
                        
                        ComPtr<ID3D11ShaderResourceView> tempSRV;
                        if (SUCCEEDED(g_device->CreateShaderResourceView(tempBuffer.Get(), &srvDesc, &tempSRV))) {
                            g_sharedFenceIn = tempFence;
                            g_sharedBufferIn = tempBuffer;
                            g_sharedBufferSRV = tempSRV;
                            g_producerManifest = *manifestView;

                            g_isConnected = true;
                            g_producerPid = pe32.th32ProcessID;
                            g_lastSeenFrame = 0;
                            UpdateWindowTitle();
                        }
                    }
                    
                    if(hFence) CloseHandle(hFence);
                    if(hBuffer) CloseHandle(hBuffer);
                    UnmapViewOfFile(manifestView);
                }
                CloseHandle(hManifest);

                if (g_isConnected) {
                    CloseHandle(hSnapshot);
                    return;
                }
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
    g_sharedBufferSRV.Reset();
    UpdateWindowTitle();
}

void UpdateWindowTitle() {
    std::wstring title;
    if (g_isConnected) {
        title = L"Buffer To Texture D3D11 - Connected to PID " + std::to_wstring(g_producerPid);
    } else {
        title = L"Buffer To Texture D3D11 - Searching for producer...";
    }
    SetWindowTextW(g_hwnd, title.c_str());
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        if (g_swapChain && wParam != SIZE_MINIMIZED) {
            g_renderTargetView.Reset();
            g_swapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateSwapchainResources();
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}