#include "pch.h"
#include "GraphicsCapture.h"
#include <dwmapi.h>
#include <d3dcompiler.h>
#include <inspectable.h>
#include <roapi.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <windows.graphics.capture.interop.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowsapp.lib")

struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
IDirect3DDxgiInterfaceAccess : ::IUnknown
{
    virtual HRESULT __stdcall GetInterface(GUID const& id, void** value) = 0;
};

using namespace Microsoft::WRL;
namespace winrt
{
    using namespace Windows::Foundation;
    using namespace Windows::Graphics::Capture;
    using namespace Windows::Graphics::DirectX;
    using namespace Windows::Graphics::DirectX::Direct3D11;
}

static std::unique_ptr<GraphicsCapture> g_graphicsCapture;

struct GraphicsCapture::Impl
{
    ~Impl() { StopCaptureInternal(); }

    void StopCaptureInternal();

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11Device5> m_device5;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<ID3D11DeviceContext4> m_context4;

    ComPtr<ID3D11Texture2D> m_privateTexture;
    ComPtr<ID3D11Texture2D> m_sharedTexture;
    ComPtr<ID3D11Fence> m_sharedFence;
    HANDLE m_sharedTextureHandle = nullptr;
    HANDLE m_sharedFenceHandle = nullptr;
    HANDLE m_hManifest = nullptr;
    BroadcastManifest* m_pManifestView = nullptr;
    UINT64 m_frameValue = 0;

    winrt::IDirect3DDevice m_winrtDevice{ nullptr };
    winrt::GraphicsCaptureItem m_captureItem{ nullptr };
    winrt::Direct3D11CaptureFramePool m_framePool{ nullptr };
    winrt::GraphicsCaptureSession m_session{ nullptr };
    winrt::Direct3D11CaptureFramePool::FrameArrived_revoker m_frameArrived;

    bool m_isCapturing = false;
    HWND m_capturedHwnd = nullptr;

    HRESULT CreateSharedResources(UINT width, UINT height, const std::wstring& manifestPrefix);
    void OnFrameArrived(winrt::Direct3D11CaptureFramePool const& sender, winrt::Windows::Foundation::IInspectable const& args);
};

GraphicsCapture::GraphicsCapture() : pImpl(std::make_unique<Impl>()) {}
GraphicsCapture::~GraphicsCapture() { Shutdown(); }

HRESULT GraphicsCapture::Initialize(ID3D11Device* device)
{
    pImpl->m_device = device;
    RETURN_IF_FAILED(pImpl->m_device.As(&pImpl->m_device5));
    pImpl->m_device->GetImmediateContext(&pImpl->m_context);
    RETURN_IF_FAILED(pImpl->m_context.As(&pImpl->m_context4));

    ComPtr<IDXGIDevice> dxgiDevice;
    RETURN_IF_FAILED(pImpl->m_device.As(&dxgiDevice));

    IInspectable* inspectableDevice = nullptr;
    RETURN_IF_FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), &inspectableDevice));
    winrt::copy_from_abi(pImpl->m_winrtDevice, inspectableDevice);

    return S_OK;
}

void GraphicsCapture::Shutdown()
{
    pImpl->StopCaptureInternal();
}

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    if (GetWindowTextLength(hwnd) == 0) return TRUE;

    std::wstring title(GetWindowTextLength(hwnd) + 1, L'\0');
    GetWindowTextW(hwnd, &title[0], (int)title.size());
    title.resize(wcslen(title.c_str()));

    if (title == L"Program Manager" || title == L"VirtuaCam" || title == L"VirtuaCam Preview") return TRUE;

    int isCloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &isCloaked, sizeof(isCloaked));
    if (isCloaked) return TRUE;

    auto* windows = reinterpret_cast<std::vector<WindowInfo>*>(lParam);
    windows->push_back({ hwnd, title });
    return TRUE;
}

std::vector<WindowInfo> GraphicsCapture::EnumerateWindows()
{
    std::vector<WindowInfo> windows;
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

HRESULT GraphicsCapture::StartCapture(HWND hwnd, const std::wstring& manifestPrefix)
{
    pImpl->StopCaptureInternal();
    pImpl->m_capturedHwnd = hwnd;

    try
    {
        auto interop = winrt::get_activation_factory<winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        interop->CreateForWindow(hwnd, winrt::guid_of<winrt::GraphicsCaptureItem>(), winrt::put_abi(pImpl->m_captureItem));

        auto size = pImpl->m_captureItem.Size();
        RETURN_IF_FAILED(pImpl->CreateSharedResources(size.Width, size.Height, manifestPrefix));

        pImpl->m_framePool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(pImpl->m_winrtDevice, winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        pImpl->m_session = pImpl->m_framePool.CreateCaptureSession(pImpl->m_captureItem);
        pImpl->m_frameArrived = pImpl->m_framePool.FrameArrived(winrt::auto_revoke, { pImpl.get(), &GraphicsCapture::Impl::OnFrameArrived });
        pImpl->m_session.StartCapture();
        pImpl->m_isCapturing = true;
    }
    catch (winrt::hresult_error const& e)
    {
        pImpl->StopCaptureInternal();
        return e.code();
    }
    return S_OK;
}

void GraphicsCapture::StopCapture()
{
    pImpl->StopCaptureInternal();
}

void GraphicsCapture::Impl::StopCaptureInternal()
{
    if (!m_isCapturing) return;

    m_frameArrived.revoke();
    if(m_session) m_session.Close();
    if(m_framePool) m_framePool.Close();

    m_captureItem = nullptr;
    m_session = nullptr;
    m_framePool = nullptr;
    m_isCapturing = false;
    m_capturedHwnd = nullptr;

    if (m_pManifestView) UnmapViewOfFile(m_pManifestView);
    if (m_hManifest) CloseHandle(m_hManifest);
    m_pManifestView = nullptr;
    m_hManifest = nullptr;

    if (m_sharedTextureHandle) CloseHandle(m_sharedTextureHandle);
    if (m_sharedFenceHandle) CloseHandle(m_sharedFenceHandle);
    m_sharedTextureHandle = nullptr;
    m_sharedFenceHandle = nullptr;

    m_privateTexture.Reset();
    m_sharedTexture.Reset();
    m_sharedFence.Reset();
}

bool GraphicsCapture::IsActive() const { return pImpl->m_isCapturing; }
HWND GraphicsCapture::GetCapturedHwnd() const { return pImpl->m_capturedHwnd; }

HRESULT GraphicsCapture::Impl::CreateSharedResources(UINT width, UINT height, const std::wstring& manifestPrefix)
{
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width; td.Height = height; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    
    td.BindFlags = 0;
    RETURN_IF_FAILED(m_device->CreateTexture2D(&td, nullptr, &m_privateTexture));
    
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    RETURN_IF_FAILED(m_device->CreateTexture2D(&td, nullptr, &m_sharedTexture));

    RETURN_IF_FAILED(m_device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_sharedFence)));

    std::wstring manifestName = manifestPrefix + std::to_wstring(GetCurrentProcessId());
    wil::unique_hlocal_security_descriptor sd;
    PSECURITY_DESCRIPTOR sd_ptr = nullptr;
    THROW_IF_WIN32_BOOL_FALSE(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd_ptr, NULL));
    sd.reset(sd_ptr);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd.get(), FALSE };
    m_hManifest = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(BroadcastManifest), manifestName.c_str());
    if (!m_hManifest) return HRESULT_FROM_WIN32(GetLastError());

    m_pManifestView = (BroadcastManifest*)MapViewOfFile(m_hManifest, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BroadcastManifest));
    if (!m_pManifestView) return HRESULT_FROM_WIN32(GetLastError());

    ZeroMemory(m_pManifestView, sizeof(BroadcastManifest));
    m_pManifestView->width = width;
    m_pManifestView->height = height;
    m_pManifestView->format = DXGI_FORMAT_B8G8R8A8_UNORM;
    ComPtr<IDXGIDevice> dxgiDevice;
    m_device.As(&dxgiDevice);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC desc;
    adapter->GetDesc(&desc);
    m_pManifestView->adapterLuid = desc.AdapterLuid;

    std::wstring texName = L"Global\\VirtuaCam_CaptureTexture_" + std::to_wstring(GetCurrentProcessId());
    std::wstring fenceName = L"Global\\VirtuaCam_CaptureFence_" + std::to_wstring(GetCurrentProcessId());

    wcscpy_s(m_pManifestView->textureName, _countof(m_pManifestView->textureName), texName.c_str());
    wcscpy_s(m_pManifestView->fenceName, _countof(m_pManifestView->fenceName), fenceName.c_str());

    ComPtr<IDXGIResource1> r1;
    m_sharedTexture.As(&r1);
    r1->CreateSharedHandle(&sa, GENERIC_ALL, texName.c_str(), &m_sharedTextureHandle);

    m_sharedFence->CreateSharedHandle(&sa, GENERIC_ALL, fenceName.c_str(), &m_sharedFenceHandle);

    return S_OK;
}

void GraphicsCapture::Impl::OnFrameArrived(winrt::Direct3D11CaptureFramePool const& sender, winrt::Windows::Foundation::IInspectable const& args)
{
    try
    {
        auto frame = sender.TryGetNextFrame();
        if (!frame) return;

        ComPtr<ID3D11Texture2D> sourceTexture;
        auto access = frame.Surface().as<IDirect3DDxgiInterfaceAccess>();
        access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), reinterpret_cast<void**>(sourceTexture.GetAddressOf()));

        if (m_privateTexture && m_sharedTexture && sourceTexture)
        {
            m_context->CopyResource(m_privateTexture.Get(), sourceTexture.Get());
            m_context->CopyResource(m_sharedTexture.Get(), m_privateTexture.Get());
            
            m_frameValue++;
            m_context4->Signal(m_sharedFence.Get(), m_frameValue);
            if (m_pManifestView) {
                InterlockedExchange64(reinterpret_cast<volatile LONGLONG*>(&m_pManifestView->frameValue), m_frameValue);
            }
        }
    }
    catch (winrt::hresult_error const& e)
    {
        if (e.code() == DXGI_ERROR_DEVICE_REMOVED || e.code() == DXGI_ERROR_DEVICE_RESET)
        {
            StopCaptureInternal();
        }
    }
}

#ifdef MFCAPTURE_EXPORTS
extern "C" {
    MFCAPTURE_API HRESULT InitializeProducer(HWND hwnd, const wchar_t* manifestPrefix)
    {
        g_graphicsCapture = std::make_unique<GraphicsCapture>();
        ComPtr<ID3D11Device> device;
        RETURN_IF_FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr));
        RETURN_IF_FAILED(g_graphicsCapture->Initialize(device.Get()));
        return g_graphicsCapture->StartCapture(hwnd, manifestPrefix);
    }

    MFCAPTURE_API void RunProducer()
    {
        if (g_graphicsCapture && g_graphicsCapture->IsActive())
        {
            MSG msg;
            while (GetMessage(&msg, NULL, 0, 0))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
    }

    MFCAPTURE_API void ShutdownProducer()
    {
        if (g_graphicsCapture)
        {
            g_graphicsCapture->Shutdown();
            g_graphicsCapture.reset();
        }
    }
}
#endif