// =============================================================================
// CameraSource.h  --  Media Foundation virtual camera source
// =============================================================================
// Object graph created by the Windows Camera Frame Server:
//
//   CameraActivate (IMFActivate, registered CLSID)
//     └─ CameraSource (IMFMediaSource2 + allocator control)
//          └─ CameraStream (IMFMediaStream2, one video stream)
//               └─ CameraRenderer (broker frame -> sample)
//
// Samples are produced on demand: the frame server calls RequestSample at the
// rate of the media type it negotiated, and each request is answered with the
// newest broker frame, time-stamped with the negotiated frame duration.
// =============================================================================

#pragma once

#include "AttributeStore.h"
#include "CameraRenderer.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfvirtualcamera.h>
#include <ks.h>
#include <ksproxy.h>
#include <wrl/implements.h>
#include <wil/resource.h>

class CameraSource;

// -----------------------------------------------------------------------------
class CameraStream final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::ChainInterfaces<IMFMediaStream2, IMFMediaStream, IMFMediaEventGenerator>>
{
public:
    HRESULT RuntimeClassInitialize(CameraSource* source, DWORD streamId);

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extended, HRESULT status, const PROPVARIANT* value) override;

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** source) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** descriptor) override;
    STDMETHODIMP RequestSample(IUnknown* token) override;

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE state) override;
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* state) override;

    // Called by CameraSource (under its lock).
    HRESULT Start(IMFMediaType* type);
    HRESULT Stop();
    HRESULT SetMediaType(IMFMediaType* type);
    HRESULT SetAllocator(IUnknown* allocator);
    HRESULT SetDeviceManager(IUnknown* manager);
    void Shutdown();
    IMFAttributes* Attributes() const { return m_attributes.get(); }
    DWORD Id() const { return m_id; }
    bool IsActive() const { return m_state != MF_STREAM_STATE_STOPPED; }

private:
    HRESULT ApplyMediaType(IMFMediaType* type);
    HRESULT BuildDescriptor();

    wil::srwlock m_lock;
    DWORD m_id = 0;
    MF_STREAM_STATE m_state = MF_STREAM_STATE_STOPPED;
    bool m_shutdown = false;
    wil::com_ptr_nothrow<IMFMediaSource> m_source;   // released in Shutdown() to break the cycle
    wil::com_ptr_nothrow<IMFAttributes> m_attributes;
    wil::com_ptr_nothrow<IMFMediaEventQueue> m_events;
    wil::com_ptr_nothrow<IMFStreamDescriptor> m_descriptor;
    wil::com_ptr_nothrow<IMFVideoSampleAllocatorEx> m_allocator;
    wil::com_ptr_nothrow<IMFDXGIDeviceManager> m_deviceManager;
    wil::com_ptr_nothrow<IMFMediaType> m_currentType;
    LONGLONG m_frameDuration = 333333;
    CameraRenderer m_renderer;
};

// -----------------------------------------------------------------------------
class CameraSource final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::ChainInterfaces<IMFMediaSource2, IMFMediaSourceEx, IMFMediaSource, IMFMediaEventGenerator>,
          IMFGetService, IKsControl, IMFSampleAllocatorControl>
{
public:
    HRESULT RuntimeClassInitialize(IMFAttributes* activationAttributes);

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extended, HRESULT status, const PROPVARIANT* value) override;

    // IMFMediaSource
    STDMETHODIMP GetCharacteristics(DWORD* characteristics) override;
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) override;
    STDMETHODIMP Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat, const PROPVARIANT* startPosition) override;
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** attributes) override;
    STDMETHODIMP GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) override;
    STDMETHODIMP SetD3DManager(IUnknown* manager) override;

    // IMFMediaSource2
    STDMETHODIMP SetMediaType(DWORD streamId, IMFMediaType* type) override;

    // IMFGetService
    STDMETHODIMP GetService(REFGUID service, REFIID riid, LPVOID* object) override;

    // IMFSampleAllocatorControl
    STDMETHODIMP SetDefaultAllocator(DWORD streamId, IUnknown* allocator) override;
    STDMETHODIMP GetAllocatorUsage(DWORD streamId, DWORD* inputStreamId, MFSampleAllocatorUsage* usage) override;

    // IKsControl: no custom properties, methods or events.
    STDMETHOD(KsProperty)(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override;
    STDMETHOD(KsMethod)(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override;
    STDMETHOD(KsEvent)(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override;

private:
    HRESULT CheckAlive() const { return m_events ? S_OK : MF_E_SHUTDOWN; }
    CameraStream* StreamById(DWORD id) const { return (m_stream && m_stream->Id() == id) ? m_stream.Get() : nullptr; }

    wil::srwlock m_lock;
    wil::com_ptr_nothrow<IMFAttributes> m_attributes;
    wil::com_ptr_nothrow<IMFMediaEventQueue> m_events;
    wil::com_ptr_nothrow<IMFPresentationDescriptor> m_presentation;
    Microsoft::WRL::ComPtr<CameraStream> m_stream;
};

// -----------------------------------------------------------------------------
class CameraActivate final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::ChainInterfaces<IMFActivate, IMFAttributes>>
{
public:
    HRESULT RuntimeClassInitialize();

    // IMFAttributes
    VCAM_FORWARD_IMFATTRIBUTES(m_attributes)

    // IMFActivate
    STDMETHODIMP ActivateObject(REFIID riid, void** object) override;
    STDMETHODIMP ShutdownObject() override;
    STDMETHODIMP DetachObject() override;

private:
    wil::critical_section m_lock;
    wil::com_ptr_nothrow<IMFAttributes> m_attributes;
    Microsoft::WRL::ComPtr<CameraSource> m_source;
};
