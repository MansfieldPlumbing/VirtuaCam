// =============================================================================
// CaptureProducer.cpp  --  Window or display -> shared frame
// =============================================================================
// Uses Windows.Graphics.Capture through its COM ABI (no C++/WinRT).  The
// free-threaded frame pool raises FrameArrived on a thread-pool thread for
// each new frame; the handler copies it into the shared texture and
// publishes.  A static window produces no frames and costs nothing.
//
// Display capture works for any monitor Windows knows about -- including
// virtual monitors created by an indirect-display driver -- which is how a
// "virtual display" feeds the camera without VirtuaCam shipping a driver.
// =============================================================================

#include "Producers.h"
#include "Gpu.h"
#include "SharedFrame.h"

#include <windows.foundation.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/event.h>
#include <wrl/wrappers/corewrappers.h>
#include <algorithm>
#include <functional>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
namespace WGC = ABI::Windows::Graphics::Capture;
namespace WGDX = ABI::Windows::Graphics::DirectX;
namespace WF = ABI::Windows::Foundation;

namespace {

// Declared in windows.graphics.directx.direct3d11.interop.h on newer SDKs
// inside a namespace; redeclared here so any SDK works.
struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")) SurfaceInterfaceAccess : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void** object) = 0;
};

template <typename T>
void CloseObject(const ComPtr<T>& object)
{
    ComPtr<WF::IClosable> closable;
    if (object && SUCCEEDED(object.As(&closable)))
        closable->Close();
}

class CaptureSession {
public:
    HRESULT Start(ComPtr<WGC::IGraphicsCaptureItem> item, DWORD mainThread);
    void Stop();

private:
    HRESULT OnFrame();
    HRESULT OpenPublisher(UINT width, UINT height);

    wil::critical_section m_lock;
    bool m_stopped = false;
    DWORD m_mainThread = 0;
    wil::com_ptr_nothrow<ID3D11Device> m_device;
    wil::com_ptr_nothrow<ID3D11DeviceContext> m_context;
    ComPtr<WGDX::Direct3D11::IDirect3DDevice> m_winrtDevice;
    ComPtr<WGC::IGraphicsCaptureItem> m_item;
    ComPtr<WGC::IDirect3D11CaptureFramePool> m_pool;
    ComPtr<WGC::IGraphicsCaptureSession> m_session;
    EventRegistrationToken m_frameToken{};
    EventRegistrationToken m_closedToken{};
    ABI::Windows::Graphics::SizeInt32 m_poolSize{};
    Ipc::FramePublisher m_publisher;
    UINT m_generation = 0;
};

HRESULT CaptureSession::OpenPublisher(UINT width, UINT height)
{
    if (!m_publisher.IsOpen())
        return m_publisher.OpenForProcess(m_device.get(), width, height);
    // Resized source: new object names, because the broker may still hold the
    // previous ones open.  Consumers find the new names in the manifest.
    const std::wstring suffix = std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++m_generation);
    return m_publisher.Recreate(width, height, L"Local\\DirectPortTexture_" + suffix, L"Local\\DirectPortFence_" + suffix);
}

HRESULT CaptureSession::Start(ComPtr<WGC::IGraphicsCaptureItem> item, DWORD mainThread)
{
    m_item = std::move(item);
    m_mainThread = mainThread;
    RETURN_IF_FAILED(Gpu::CreateDevice(&m_device));
    m_device->GetImmediateContext(&m_context);

    wil::com_ptr_nothrow<IDXGIDevice> dxgi;
    RETURN_IF_FAILED(m_device->QueryInterface(IID_PPV_ARGS(&dxgi)));
    ComPtr<IInspectable> inspectable;
    RETURN_IF_FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), &inspectable));
    RETURN_IF_FAILED(inspectable.As(&m_winrtDevice));

    RETURN_IF_FAILED(m_item->get_Size(&m_poolSize));
    RETURN_IF_FAILED(OpenPublisher((UINT)m_poolSize.Width, (UINT)m_poolSize.Height));

    ComPtr<WGC::IDirect3D11CaptureFramePoolStatics2> poolStatics;
    RETURN_IF_FAILED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool).Get(),
                                            IID_PPV_ARGS(&poolStatics)));
    RETURN_IF_FAILED(poolStatics->CreateFreeThreaded(m_winrtDevice.Get(), WGDX::DirectXPixelFormat_B8G8R8A8UIntNormalized,
                                                     2, m_poolSize, &m_pool));
    RETURN_IF_FAILED(m_pool->CreateCaptureSession(m_item.Get(), &m_session));

    // Best effort, newer Windows only: no yellow capture border, keep cursor.
    ComPtr<WGC::IGraphicsCaptureSession3> session3;
    if (SUCCEEDED(m_session.As(&session3)))
        session3->put_IsBorderRequired(false);

    RETURN_IF_FAILED(m_pool->add_FrameArrived(
        Callback<WF::ITypedEventHandler<WGC::Direct3D11CaptureFramePool*, IInspectable*>>(
            [this](WGC::IDirect3D11CaptureFramePool*, IInspectable*) { OnFrame(); return S_OK; }).Get(),
        &m_frameToken));
    RETURN_IF_FAILED(m_item->add_Closed(
        Callback<WF::ITypedEventHandler<WGC::GraphicsCaptureItem*, IInspectable*>>(
            [this](WGC::IGraphicsCaptureItem*, IInspectable*) {
                PostThreadMessageW(m_mainThread, WM_QUIT, ProducerExitSourceLost, 0);   // window closed
                return S_OK;
            }).Get(),
        &m_closedToken));
    return m_session->StartCapture();
}

HRESULT CaptureSession::OnFrame()
{
    auto lock = m_lock.lock();
    if (m_stopped)
        return S_OK;

    ComPtr<WGC::IDirect3D11CaptureFrame> frame;
    RETURN_IF_FAILED(m_pool->TryGetNextFrame(&frame));
    if (!frame)
        return S_OK;
    auto close = wil::scope_exit([&] { CloseObject(frame); });

    ABI::Windows::Graphics::SizeInt32 size{};
    RETURN_IF_FAILED(frame->get_ContentSize(&size));
    if (size.Width <= 0 || size.Height <= 0)
        return S_OK;   // minimised

    if (size.Width != m_poolSize.Width || size.Height != m_poolSize.Height) {
        // Source resized: resize the pool and the shared texture, then take
        // the next frame at the new size.
        m_poolSize = size;
        RETURN_IF_FAILED(m_pool->Recreate(m_winrtDevice.Get(), WGDX::DirectXPixelFormat_B8G8R8A8UIntNormalized, 2, size));
        return OpenPublisher((UINT)size.Width, (UINT)size.Height);
    }

    ComPtr<WGDX::Direct3D11::IDirect3DSurface> surface;
    RETURN_IF_FAILED(frame->get_Surface(&surface));
    ComPtr<SurfaceInterfaceAccess> access;
    RETURN_IF_FAILED(surface.As(&access));
    wil::com_ptr_nothrow<ID3D11Texture2D> texture;
    RETURN_IF_FAILED(access->GetInterface(IID_PPV_ARGS(&texture)));

    const D3D11_BOX box{ 0, 0, 0, std::min<UINT>(size.Width, m_publisher.Width()), std::min<UINT>(size.Height, m_publisher.Height()), 1 };
    m_context->CopySubresourceRegion(m_publisher.Texture(), 0, 0, 0, 0, texture.get(), 0, &box);
    m_publisher.Publish(m_context.get());
    return S_OK;
}

void CaptureSession::Stop()
{
    {
        auto lock = m_lock.lock();
        m_stopped = true;
    }
    if (m_pool && m_frameToken.value)
        m_pool->remove_FrameArrived(m_frameToken);
    if (m_item && m_closedToken.value)
        m_item->remove_Closed(m_closedToken);
    CloseObject(m_session);
    CloseObject(m_pool);
    m_session.Reset();
    m_pool.Reset();
    m_item.Reset();
    auto lock = m_lock.lock();
    m_publisher.Close();
}

int RunCapture(const std::function<HRESULT(IGraphicsCaptureItemInterop*, WGC::IGraphicsCaptureItem**)>& createItem)
{
    // The host thread is already in the MTA (see RunProducer).
    ComPtr<IGraphicsCaptureItemInterop> interop;
    if (FAILED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem).Get(),
                                      IID_PPV_ARGS(&interop))))
        return ProducerExitStartFailed;
    ComPtr<WGC::IGraphicsCaptureItem> item;
    if (FAILED(createItem(interop.Get(), &item)))
        return ProducerExitStartFailed;

    CaptureSession session;
    if (FAILED(session.Start(item, GetCurrentThreadId()))) {
        session.Stop();
        return ProducerExitStartFailed;
    }
    const int code = RunMessageLoop();
    session.Stop();
    return code;
}

} // namespace

int RunWindowCaptureProducer(HWND window)
{
    if (!IsWindow(window))
        return ProducerExitBadArguments;
    return RunCapture([window](IGraphicsCaptureItemInterop* interop, WGC::IGraphicsCaptureItem** item) {
        return interop->CreateForWindow(window, IID_PPV_ARGS(item));
    });
}

int RunDisplayCaptureProducer(const std::wstring& deviceName)
{
    struct Search { const std::wstring* name; HMONITOR found; } search{ &deviceName, nullptr };
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        auto* s = reinterpret_cast<Search*>(data);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info) && *s->name == info.szDevice) {
            s->found = monitor;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    if (!search.found)
        return ProducerExitBadArguments;

    const HMONITOR monitor = search.found;
    return RunCapture([monitor](IGraphicsCaptureItemInterop* interop, WGC::IGraphicsCaptureItem** item) {
        return interop->CreateForMonitor(monitor, IID_PPV_ARGS(item));
    });
}
