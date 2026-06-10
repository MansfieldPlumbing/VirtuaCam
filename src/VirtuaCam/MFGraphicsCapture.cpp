// =============================================================================
// MFGraphicsCapture.cpp  --  Window / desktop capture producer
// =============================================================================
// This producer DLL is loaded by VirtuaCamProcess.exe (--type capture).
// It captures a target window (or the desktop) using the Windows.Graphics.Capture
// API (introduced in Windows 10 1803) and forwards frames to the broker.
//
// Argument: --hwnd <handle-as-uint64>   (the window to capture)
//
// Windows.Graphics.Capture vs. Desktop Duplication
// -------------------------------------------------
// The older DXGI Desktop Duplication API requires capturing the whole desktop;
// Windows.Graphics.Capture can target an individual window, handles DWM
// composition correctly (captures what you see on screen, including rounded
// corners), and works without administrator privileges.
//
// Raw ABI access (no C++/WinRT)
// -----------------------------
// Windows.Graphics.Capture is a WinRT API, but WinRT objects are just COM
// objects underneath.  This file talks to the API at the ABI level: activation
// factories are obtained with RoGetActivationFactory() and all calls go through
// the ABI::Windows::Graphics::Capture::* COM interfaces from the Windows SDK
// headers.  No C++/WinRT projection (and no vcpkg cppwinrt package) is needed.
//
// IDirect3DDxgiInterfaceAccess
// ----------------------------
// Direct3D11CaptureFrame::Surface() returns an IDirect3DSurface (a WinRT
// wrapper).  To get the underlying ID3D11Texture2D we must QI through
// IDirect3DDxgiInterfaceAccess::GetInterface().  This interface is declared
// inline here with a guard to avoid double-definition if a newer SDK ships it.
// =============================================================================

#include "pch.h"
#include "MFGraphicsCapture.h"
#include "Tools.h"
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>
#include <sddl.h>
#include <string>
#include <sstream>
#include <atomic>
#include <roapi.h>
#include <windows.foundation.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.directx.direct3d11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <inspectable.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "runtimeobject.lib")

using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;
namespace WGC = ABI::Windows::Graphics::Capture;
namespace WGD = ABI::Windows::Graphics::DirectX;
namespace WGD3D = ABI::Windows::Graphics::DirectX::Direct3D11;

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

static ComPtr<WGD3D::IDirect3DDevice> g_d3dDevice;
static ComPtr<WGC::IGraphicsCaptureItem> g_captureItem;
static ComPtr<WGC::IDirect3D11CaptureFramePool> g_framePool;
static ComPtr<WGC::IGraphicsCaptureSession> g_session;

static std::atomic<bool> g_isCapturing = false;
static bool g_needRoUninit = false;

HRESULT InitD3D11() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    RETURN_IF_FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &g_d3d11Device, nullptr, &g_d3d11Context));
    RETURN_IF_FAILED(g_d3d11Device.As(&g_d3d11Device5));
    RETURN_IF_FAILED(g_d3d11Context.As(&g_d3d11Context4));
    return S_OK;
}

// Closes a WinRT object through its IClosable interface (the ABI equivalent
// of calling Close() on the projection).  Safe to call with a null pointer.
template <typename T>
static void CloseWinRTObject(const ComPtr<T>& object)
{
    if (!object) return;
    ComPtr<ABI::Windows::Foundation::IClosable> closable;
    if (SUCCEEDED(object.As(&closable)))
        closable->Close();
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

        // WinRT activation requires the runtime to be initialized on this
        // thread.  The host already enters the MTA via CoInitializeEx, so
        // RO_INIT_MULTITHREADED is compatible (and may return S_FALSE).
        HRESULT hrInit = RoInitialize(RO_INIT_MULTITHREADED);
        if (FAILED(hrInit) && hrInit != RPC_E_CHANGED_MODE)
            return hrInit;
        g_needRoUninit = SUCCEEDED(hrInit);

        RETURN_IF_FAILED(InitD3D11());

        // GraphicsCaptureItem from HWND, via the interop factory.
        ComPtr<IGraphicsCaptureItemInterop> interopFactory;
        RETURN_IF_FAILED(RoGetActivationFactory(
            HStringReference(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem).Get(),
            IID_PPV_ARGS(&interopFactory)));
        RETURN_IF_FAILED(interopFactory->CreateForWindow(hwndToCapture, IID_PPV_ARGS(&g_captureItem)));

        // Wrap our D3D11 device in a WinRT IDirect3DDevice for the frame pool.
        ComPtr<IDXGIDevice> dxgiDevice;
        RETURN_IF_FAILED(g_d3d11Device.As(&dxgiDevice));
        ComPtr<IInspectable> inspectableDevice;
        RETURN_IF_FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), &inspectableDevice));
        RETURN_IF_FAILED(inspectableDevice.As(&g_d3dDevice));

        ABI::Windows::Graphics::SizeInt32 size{};
        RETURN_IF_FAILED(g_captureItem->get_Size(&size));

        // Free-threaded frame pool (no Dispatcher needed; we poll from the
        // producer's frame loop).
        ComPtr<WGC::IDirect3D11CaptureFramePoolStatics2> framePoolStatics;
        RETURN_IF_FAILED(RoGetActivationFactory(
            HStringReference(RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool).Get(),
            IID_PPV_ARGS(&framePoolStatics)));
        RETURN_IF_FAILED(framePoolStatics->CreateFreeThreaded(
            g_d3dDevice.Get(),
            WGD::DirectXPixelFormat::DirectXPixelFormat_B8G8R8A8UIntNormalized,
            2, size, &g_framePool));

        RETURN_IF_FAILED(g_framePool->CreateCaptureSession(g_captureItem.Get(), &g_session));

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

        ComPtr<IDXGIAdapter> adapter; dxgiDevice->GetAdapter(&adapter);
        DXGI_ADAPTER_DESC desc; adapter->GetDesc(&desc);
        g_pManifestView->adapterLuid = desc.AdapterLuid;

        wcscpy_s(g_pManifestView->textureName, texName.c_str());
        wcscpy_s(g_pManifestView->fenceName, fenceName.c_str());

        ComPtr<IDXGIResource1> r1; g_sharedD3D11Texture.As(&r1);
        RETURN_IF_FAILED(r1->CreateSharedHandle(&sa, GENERIC_ALL, texName.c_str(), &g_hSharedTextureHandle));
        RETURN_IF_FAILED(g_sharedD3D11Fence->CreateSharedHandle(&sa, GENERIC_ALL, fenceName.c_str(), &g_hSharedFenceHandle));

        RETURN_IF_FAILED(g_session->StartCapture());
        g_isCapturing = true;
        return S_OK;
    }

    PRODUCER_API void ProcessFrame()
    {
        if (!g_isCapturing || !g_framePool) return;

        // TryGetNextFrame() is non-blocking — returns null if no new frame
        // is ready yet.  The caller (Process.cpp) re-polls every ~1 ms.
        ComPtr<WGC::IDirect3D11CaptureFrame> frame;
        if (FAILED(g_framePool->TryGetNextFrame(&frame)))
        {
            g_isCapturing = false;
            return;
        }
        if (!frame) return;

        // Unwrap the WinRT IDirect3DSurface to the underlying D3D11 texture
        // via IDirect3DDxgiInterfaceAccess (see file header for why this is needed).
        ComPtr<WGD3D::IDirect3DSurface> surface;
        if (SUCCEEDED(frame->get_Surface(&surface)))
        {
            ComPtr<IDirect3DDxgiInterfaceAccess> surfaceAccess;
            if (SUCCEEDED(surface.As(&surfaceAccess)))
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
        }

        CloseWinRTObject(frame);
    }

    PRODUCER_API void ShutdownProducer()
    {
        if (!g_isCapturing.exchange(false)) return;

        CloseWinRTObject(g_session);
        g_session.Reset();
        CloseWinRTObject(g_framePool);
        g_framePool.Reset();
        g_captureItem.Reset();
        g_d3dDevice.Reset();

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

        if (g_needRoUninit) {
            RoUninitialize();
            g_needRoUninit = false;
        }
    }
}
