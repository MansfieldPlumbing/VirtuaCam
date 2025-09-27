#include "pch.h"
#include "MFGraphicsCapture.h"
#include "Tools.h"
#include <wrl.h>
#include <sddl.h>
#include <string>
#include <sstream>
#include <atomic>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <inspectable.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "advapi32.lib")

using namespace Microsoft::WRL;
namespace winrt
{
    using namespace Windows::Foundation;
    using namespace Windows::Graphics::Capture;
    using namespace Windows::Graphics::DirectX;
    using namespace Windows::Graphics::DirectX::Direct3D11;
}

#ifndef __IDirect3DDxgiInterfaceAccess_declared
#define __IDirect3DDxgiInterfaceAccess_declared
struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")) IDirect3DDxgiInterfaceAccess : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID riid, void **ppvObject) = 0;
};
#endif

static ComPtr<ID3D11Device> g_d3d11Device;
static ComPtr<ID3D11Device5> g_d3d11Device5;
static ComPtr<ID3D11DeviceContext> g_d3d11Context;
static ComPtr<ID3D11DeviceContext4> g_d3d11Context4;
static ComPtr<ID3D11Texture2D> g_sharedD3D11Texture;
static ComPtr<ID3D11Fence> g_sharedD3D11Fence;
static HANDLE g_hSharedTextureHandle = nullptr;
static HANDLE g_hSharedFenceHandle = nullptr;
static HANDLE g_hManifest = nullptr;
static BroadcastManifest* g_pManifestView = nullptr;
static std::atomic<UINT64> g_fenceValue = 0;

static winrt::IDirect3DDevice g_d3dDevice{ nullptr };
static winrt::GraphicsCaptureItem g_captureItem{ nullptr };
static winrt::Direct3D11CaptureFramePool g_framePool{ nullptr };
static winrt::GraphicsCaptureSession g_session{ nullptr };

static std::atomic<bool> g_isCapturing = false;

HRESULT InitD3D11() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    RETURN_IF_FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &g_d3d11Device, nullptr, &g_d3d11Context));
    RETURN_IF_FAILED(g_d3d11Device.As(&g_d3d11Device5));
    RETURN_IF_FAILED(g_d3d11Context.As(&g_d3d11Context4));
    return S_OK;
}

extern "C" {
    PRODUCER_API HRESULT InitializeProducer(const wchar_t* args)
    {
        HWND hwndToCapture = nullptr;
        std::wistringstream iss(args);
        std::wstring key;
        while(iss >> key) {
            if(key == L"--hwnd") {
                UINT64 hwnd_val;
                iss >> hwnd_val;
                hwndToCapture = reinterpret_cast<HWND>(hwnd_val);
            }
        }
        RETURN_HR_IF_NULL(E_INVALIDARG, hwndToCapture);

        RETURN_IF_FAILED(InitD3D11());

        try
        {
            auto interop_factory = winrt::get_activation_factory<winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            winrt::check_hresult(interop_factory->CreateForWindow(hwndToCapture, winrt::guid_of<winrt::GraphicsCaptureItem>(), winrt::put_abi(g_captureItem)));
            if (!g_captureItem) return E_FAIL;

            ComPtr<IDXGIDevice> dxgiDevice;
            g_d3d11Device.As(&dxgiDevice);
            winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), reinterpret_cast<IInspectable**>(winrt::put_abi(g_d3dDevice))));

            auto size = g_captureItem.Size();
            g_framePool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(g_d3dDevice, winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
            if (!g_framePool) return E_FAIL;

            g_session = g_framePool.CreateCaptureSession(g_captureItem);
            if (!g_session) return E_FAIL;
        }
        catch (winrt::hresult_error const& ex) { return ex.code(); }

        auto size = g_captureItem.Size();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = size.Width; td.Height = size.Height; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        RETURN_IF_FAILED(g_d3d11Device->CreateTexture2D(&td, nullptr, &g_sharedD3D11Texture));
        RETURN_IF_FAILED(g_d3d11Device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_sharedD3D11Fence)));

        DWORD pid = GetCurrentProcessId();
        std::wstring manifestName = L"DirectPort_Producer_Manifest_" + std::to_wstring(pid);
        std::wstring texName = L"Global\\DirectPortTexture_" + std::to_wstring(pid);
        std::wstring fenceName = L"Global\\DirectPortFence_" + std::to_wstring(pid);

        wil::unique_hlocal_security_descriptor sd; PSECURITY_DESCRIPTOR sd_ptr = nullptr;
        THROW_IF_WIN32_BOOL_FALSE(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd_ptr, NULL));
        sd.reset(sd_ptr);
        SECURITY_ATTRIBUTES sa = { sizeof(sa), sd.get(), FALSE };
        
        g_hManifest = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(BroadcastManifest), manifestName.c_str());
        if (!g_hManifest) return HRESULT_FROM_WIN32(GetLastError());
        g_pManifestView = (BroadcastManifest*)MapViewOfFile(g_hManifest, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BroadcastManifest));
        if (!g_pManifestView) return HRESULT_FROM_WIN32(GetLastError());

        ZeroMemory(g_pManifestView, sizeof(BroadcastManifest));
        g_pManifestView->width = size.Width; g_pManifestView->height = size.Height;
        g_pManifestView->format = DXGI_FORMAT_B8G8R8A8_UNORM;

        ComPtr<IDXGIDevice> dxgiDevice; g_d3d11Device.As(&dxgiDevice);
        ComPtr<IDXGIAdapter> adapter; dxgiDevice->GetAdapter(&adapter);
        DXGI_ADAPTER_DESC desc; adapter->GetDesc(&desc);
        g_pManifestView->adapterLuid = desc.AdapterLuid;
        
        wcscpy_s(g_pManifestView->textureName, texName.c_str());
        wcscpy_s(g_pManifestView->fenceName, fenceName.c_str());

        ComPtr<IDXGIResource1> r1; g_sharedD3D11Texture.As(&r1);
        RETURN_IF_FAILED(r1->CreateSharedHandle(&sa, GENERIC_ALL, texName.c_str(), &g_hSharedTextureHandle));
        RETURN_IF_FAILED(g_sharedD3D11Fence->CreateSharedHandle(&sa, GENERIC_ALL, fenceName.c_str(), &g_hSharedFenceHandle));

        g_session.StartCapture();
        g_isCapturing = true;
        return S_OK;
    }

    PRODUCER_API void ProcessFrame()
    {
        if (!g_isCapturing || !g_framePool) return;

        try
        {
            winrt::Direct3D11CaptureFrame frame = g_framePool.TryGetNextFrame();
            if (frame)
            {
                auto surface = frame.Surface();
                ComPtr<IDirect3DDxgiInterfaceAccess> surfaceAccess;
                if (SUCCEEDED(surface.as<IUnknown>()->QueryInterface(IID_PPV_ARGS(&surfaceAccess))))
                {
                    ComPtr<ID3D11Texture2D> frameTexture;
                    if (SUCCEEDED(surfaceAccess->GetInterface(IID_PPV_ARGS(&frameTexture))))
                    {
                        g_d3d11Context->CopyResource(g_sharedD3D11Texture.Get(), frameTexture.Get());
                        
                        UINT64 newFenceValue = g_fenceValue.fetch_add(1) + 1;
                        g_d3d11Context4->Signal(g_sharedD3D11Fence.Get(), newFenceValue);

                        if (g_pManifestView) {
                            InterlockedExchange64(reinterpret_cast<volatile LONGLONG*>(&g_pManifestView->frameValue), newFenceValue);
                        }
                    }
                }
                
                frame.Close();
            }
        }
        catch (winrt::hresult_error const&)
        {
            g_isCapturing = false;
        }
    }

    PRODUCER_API void ShutdownProducer()
    {
        if (!g_isCapturing.exchange(false)) return;

        if (g_session) { g_session.Close(); g_session = nullptr; }
        if (g_framePool) { g_framePool.Close(); g_framePool = nullptr; }
        g_captureItem = nullptr;
        g_d3dDevice = nullptr;
        
        if (g_pManifestView) UnmapViewOfFile(g_pManifestView);
        if (g_hManifest) CloseHandle(g_hManifest);
        g_pManifestView = nullptr; g_hManifest = nullptr;

        if (g_hSharedTextureHandle) CloseHandle(g_hSharedTextureHandle);
        if (g_hSharedFenceHandle) CloseHandle(g_hSharedFenceHandle);
        g_hSharedTextureHandle = nullptr; g_hSharedFenceHandle = nullptr;

        g_sharedD3D11Fence.Reset(); g_sharedD3D11Texture.Reset();
        
        if(g_d3d11Context) g_d3d11Context->ClearState();
        g_d3d11Context4.Reset();
        g_d3d11Context.Reset();
        g_d3d11Device5.Reset();
        g_d3d11Device.Reset();
    }
}