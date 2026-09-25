// =============================================================================
// CameraRenderer.h  --  Turns the broker's shared frame into camera samples
// =============================================================================
// One renderer per camera stream.  For every sample the frame server asks for:
//
//   broker frame (BGRA, any size)  --VideoProcessorBlt-->  sample texture
//                                   scale + letterbox +     (NV12 / YUY2 /
//                                   RGB->YUV in one pass     RGB32, any size)
//
// When the frame server supplies a DXGI device manager the blit writes
// straight into the allocator's GPU texture (zero copy).  Without one the
// renderer uses its own device and reads the result back into the sample's
// memory buffer, so consumers that never negotiate D3D still work.
//
// When the broker is not running the "NO SIGNAL" frame is scaled instead.
// =============================================================================

#pragma once

#include "SharedFrame.h"

#include <mfapi.h>
#include <mfidl.h>
#include <d3d11_4.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <vector>

class CameraRenderer {
public:
    CameraRenderer() = default;
    CameraRenderer(const CameraRenderer&) = delete;
    CameraRenderer& operator=(const CameraRenderer&) = delete;
    ~CameraRenderer() { Reset(); }

    // Null selects the renderer's private device and the CPU readback path.
    HRESULT SetDeviceManager(IMFDXGIDeviceManager* manager);

    // Negotiated output: frame size and subtype (NV12, YUY2 or RGB32).
    HRESULT Configure(UINT width, UINT height, REFGUID subtype);

    // Fills the (single) buffer of a sample produced by the stream's allocator.
    HRESULT Render(IMFSample* sample);

    // Releases every device-bound object.
    void Reset();

private:
    struct OutputView {
        wil::com_ptr_nothrow<ID3D11Texture2D> texture;
        UINT subresource = 0;
        wil::com_ptr_nothrow<ID3D11VideoProcessorOutputView> view;
    };

    HRESULT AcquireDevice(ID3D11Device** device);
    void ReleaseDevice();
    void ResetDeviceObjects();
    HRESULT PrepareInput(ID3D11Device* device, ID3D11Texture2D** input);
    HRESULT PrepareProcessor(ID3D11Device* device, UINT inputWidth, UINT inputHeight);
    HRESULT OutputViewFor(ID3D11Texture2D* texture, UINT subresource, ID3D11VideoProcessorOutputView** view);
    HRESULT Blit(ID3D11Texture2D* input, ID3D11VideoProcessorOutputView* output);
    HRESULT RenderToGpuSample(ID3D11Device* device, ID3D11Texture2D* input, IMFMediaBuffer* buffer);
    HRESULT RenderToMemorySample(ID3D11Device* device, ID3D11Texture2D* input, IMFMediaBuffer* buffer);

    // Device source: the frame server's manager, or a private device.
    wil::com_ptr_nothrow<IMFDXGIDeviceManager> m_manager;
    HANDLE m_deviceHandle = nullptr;
    wil::com_ptr_nothrow<ID3D11Device> m_privateDevice;
    ID3D11Device* m_boundDevice = nullptr;   // identity of the device the objects below live on

    // Negotiated output.
    UINT m_width = 0;
    UINT m_height = 0;
    DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;

    // Input side.
    Ipc::FrameSubscription m_broker;
    ULONGLONG m_nextBrokerAttempt = 0;
    ULONGLONG m_lastBrokerFrameTick = 0;
    UINT64 m_lastBrokerFrame = 0;
    wil::com_ptr_nothrow<ID3D11Texture2D> m_noSignal;

    // Video processor, rebuilt when the input or output size changes.
    wil::com_ptr_nothrow<ID3D11VideoDevice> m_videoDevice;
    wil::com_ptr_nothrow<ID3D11VideoContext> m_videoContext;
    wil::com_ptr_nothrow<ID3D11VideoProcessorEnumerator> m_enumerator;
    wil::com_ptr_nothrow<ID3D11VideoProcessor> m_processor;
    UINT m_processorInputWidth = 0;
    UINT m_processorInputHeight = 0;
    wil::com_ptr_nothrow<ID3D11Texture2D> m_inputTexture;
    wil::com_ptr_nothrow<ID3D11VideoProcessorInputView> m_inputView;
    std::vector<OutputView> m_outputViews;

    // CPU readback path.
    wil::com_ptr_nothrow<ID3D11Texture2D> m_target;
    wil::com_ptr_nothrow<ID3D11VideoProcessorOutputView> m_targetView;
    wil::com_ptr_nothrow<ID3D11Texture2D> m_staging;
};
