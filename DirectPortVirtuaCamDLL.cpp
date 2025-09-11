#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfvirtualcamera.h>
#include <ks.h>
#include <ksmedia.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <propvarutil.h>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <memory>
#include <sddl.h>
#include <tlhelp32.h>

#include <wil/com.h>
#include <wil/result.h>
#include <wil/resource.h>
#include <wil/win32_helpers.h>
#include <winrt/base.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "mfsensorgroup.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "d3dcompiler.lib")

DEFINE_GUID(CLSID_DirectPortVirtualCamera, 0x08675309, 0x4a4e, 0x4e59, 0x86, 0x75, 0x30, 0x9a, 0x44, 0x9b, 0x00, 0x00);
DEFINE_GUID(IID_IDirectPortVirtuaCamControl, 0x90675309, 0x4a4e, 0x4e59, 0x86, 0x75, 0x30, 0x9a, 0x44, 0x9b, 0x00, 0x01);
DEFINE_GUID(MR_CAMERA_CONTROL_SERVICE, 0x90675309, 0x4a4e, 0x4e59, 0x86, 0x75, 0x30, 0x9a, 0x44, 0x9b, 0x00, 0x02);

const WCHAR VCAM_FRIENDLY_NAME[] = L"DirectPort VirtuaCam";
const WCHAR PRODUCER_MANIFEST_PREFIX[] = L"DirectPort_Producer_Manifest_";
const WCHAR PREVIEW_WND_CLASS[] = L"DirectPortVirtuaCamPreviewWnd_90675309";

const UINT32 VCAM_WIDTH = 1920;
const UINT32 VCAM_HEIGHT = 1080;
const UINT32 VCAM_FRAME_RATE_NUM = 30;
const UINT32 VCAM_FRAME_RATE_DEN = 1;

HMODULE g_module = nullptr;

interface IDirectPortVirtuaCamControl : public IUnknown
{
    STDMETHOD(TogglePreviewWindow)() = 0;
    STDMETHOD(IsPreviewWindowVisible)(BOOL* pIsVisible) = 0;
};

struct BroadcastManifest {
    UINT64 frameValue; UINT width; UINT height; DXGI_FORMAT format;
    LUID adapterLuid; WCHAR textureName[256]; WCHAR fenceName[256];
};

struct ProducerConnection
{
    bool isConnected = false;
    DWORD producerPid = 0;
    wil::unique_handle hManifest;
    wil::unique_mapview_ptr<BroadcastManifest> pManifestView;
    wil::com_ptr_nothrow<ID3D11Texture2D> sharedTexture;
    wil::com_ptr_nothrow<ID3D11Fence> sharedFence;
    UINT64 lastSeenFrame = 0;
};

class FrameGenerator;
struct MediaSource;

struct PreviewWindow
{
    HWND m_hwnd = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_shutdown = false;
    MediaSource* m_mediaSource = nullptr;

    wil::com_ptr_nothrow<ID3D11Device> m_device;
    wil::com_ptr_nothrow<ID3D11DeviceContext> m_context;
    wil::com_ptr_nothrow<IDXGISwapChain> m_swapChain;
    wil::com_ptr_nothrow<ID3D11RenderTargetView> m_rtv;

    wil::com_ptr_nothrow<ID3D11VertexShader> m_vertexShader;
    wil::com_ptr_nothrow<ID3D11PixelShader> m_pixelShader;
    wil::com_ptr_nothrow<ID3D11SamplerState> m_sampler;

    void Start(MediaSource* mediaSource);
    void Stop();
    void ThreadProc();
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    HRESULT InitD3D();
    void RenderFrame();
    void CleanupD3D();
};

class FrameGenerator
{
public:
    FrameGenerator() = default;
    ~FrameGenerator() { Stop(); }

    void Start();
    void Stop();
    
    HRESULT SetD3DManager(IUnknown* manager);
    HRESULT Generate(IMFSample* sample, REFGUID format, IMFSample** outSample);
    
    wil::com_ptr_nothrow<ID3D11Device> GetDevice() { return m_device; }
    wil::com_ptr_nothrow<ID3D11Texture2D> GetLastFrame() { winrt::slim_lock_guard lock(m_producerLock); return m_lastGoodFrame; }
    bool IsProducerConnected() { winrt::slim_lock_guard lock(m_producerLock); return m_producer.isConnected; }

private:
    void WorkerThreadProc();
    HRESULT EnsureResources();
    HRESULT CreateDeviceResources();
    HRESULT CreateNoSignalResources();
    HRESULT RenderNoSignal();
    HRESULT ConvertRGBToNV12(IMFSample* inSample, IMFSample** outSample);
    void FindAndConnectToProducer();
    void DisconnectFromProducer();
    static HRESULT GetHandleFromName(const WCHAR* name, HANDLE* handle);

    std::thread m_workerThread;
    std::atomic<bool> m_shutdownSignal = false;
    
    winrt::slim_mutex m_producerLock;
    ProducerConnection m_producer;
    wil::com_ptr_nothrow<ID3D11Texture2D> m_lastGoodFrame;

    wil::com_ptr_nothrow<IMFDXGIDeviceManager> m_dxgiManager;
    wil::unique_handle m_deviceHandle;
    wil::com_ptr_nothrow<ID3D11Device> m_device;
    wil::com_ptr_nothrow<ID3D11Device5> m_device5;
    wil::com_ptr_nothrow<ID3D11DeviceContext> m_context;
    wil::com_ptr_nothrow<ID3D11DeviceContext4> m_context4;
    LUID m_adapterLuid = {};

    wil::com_ptr_nothrow<IMFTransform> m_converter;
    
    wil::com_ptr_nothrow<ID2D1RenderTarget> m_renderTarget;
    wil::com_ptr_nothrow<ID2D1SolidColorBrush> m_textBrush;
    wil::com_ptr_nothrow<IDWriteFactory> m_dwriteFactory;
    wil::com_ptr_nothrow<IDWriteTextFormat> m_textFormat;
};

template <class IFACE = IMFAttributes>
struct CBaseAttributes : public IFACE
{
protected:
    wil::com_ptr_nothrow<IMFAttributes> m_attributes;
    CBaseAttributes() { THROW_IF_FAILED(MFCreateAttributes(&m_attributes, 0)); }
public:
    STDMETHODIMP GetItem(REFGUID g, PROPVARIANT* v) { return m_attributes->GetItem(g, v); }
    STDMETHODIMP GetItemType(REFGUID g, MF_ATTRIBUTE_TYPE* t) { return m_attributes->GetItemType(g, t); }
    STDMETHODIMP CompareItem(REFGUID g, REFPROPVARIANT c, BOOL* b) { return m_attributes->CompareItem(g, c, b); }
    STDMETHODIMP Compare(IMFAttributes* a, MF_ATTRIBUTES_MATCH_TYPE t, BOOL* b) { return m_attributes->Compare(a, t, b); }
    STDMETHODIMP GetUINT32(REFGUID g, UINT32* v) { return m_attributes->GetUINT32(g, v); }
    STDMETHODIMP GetUINT64(REFGUID g, UINT64* v) { return m_attributes->GetUINT64(g, v); }
    STDMETHODIMP GetDouble(REFGUID g, double* v) { return m_attributes->GetDouble(g, v); }
    STDMETHODIMP GetGUID(REFGUID g, GUID* v) { return m_attributes->GetGUID(g, v); }
    STDMETHODIMP GetStringLength(REFGUID g, UINT32* l) { return m_attributes->GetStringLength(g, l); }
    STDMETHODIMP GetString(REFGUID g, LPWSTR s, UINT32 l, UINT32* sl) { return m_attributes->GetString(g, s, l, sl); }
    STDMETHODIMP GetAllocatedString(REFGUID g, LPWSTR* s, UINT32* l) { return m_attributes->GetAllocatedString(g, s, l); }
    STDMETHODIMP GetBlobSize(REFGUID g, UINT32* s) { return m_attributes->GetBlobSize(g, s); }
    STDMETHODIMP GetBlob(REFGUID g, UINT8* b, UINT32 s, UINT32* bs) { return m_attributes->GetBlob(g, b, s, bs); }
    STDMETHODIMP GetAllocatedBlob(REFGUID g, UINT8** b, UINT32* s) { return m_attributes->GetAllocatedBlob(g, b, s); }
    STDMETHODIMP GetUnknown(REFGUID g, REFIID r, LPVOID* v) { return m_attributes->GetUnknown(g, r, v); }
    STDMETHODIMP SetItem(REFGUID g, REFPROPVARIANT v) { return m_attributes->SetItem(g, v); }
    STDMETHODIMP DeleteItem(REFGUID g) { return m_attributes->DeleteItem(g); }
    STDMETHODIMP DeleteAllItems() { return m_attributes->DeleteAllItems(); }
    STDMETHODIMP SetUINT32(REFGUID g, UINT32 v) { return m_attributes->SetUINT32(g, v); }
    STDMETHODIMP SetUINT64(REFGUID g, UINT64 v) { return m_attributes->SetUINT64(g, v); }
    STDMETHODIMP SetDouble(REFGUID g, double v) { return m_attributes->SetDouble(g, v); }
    STDMETHODIMP SetGUID(REFGUID g, REFGUID v) { return m_attributes->SetGUID(g, v); }
    STDMETHODIMP SetString(REFGUID g, LPCWSTR v) { return m_attributes->SetString(g, v); }
    STDMETHODIMP SetBlob(REFGUID g, const UINT8* b, UINT32 s) { return m_attributes->SetBlob(g, b, s); }
    STDMETHODIMP SetUnknown(REFGUID g, IUnknown* u) { return m_attributes->SetUnknown(g, u); }
    STDMETHODIMP LockStore() { return m_attributes->LockStore(); }
    STDMETHODIMP UnlockStore() { return m_attributes->UnlockStore(); }
    STDMETHODIMP GetCount(UINT32* c) { return m_attributes->GetCount(c); }
    STDMETHODIMP GetItemByIndex(UINT32 i, GUID* g, PROPVARIANT* v) { return m_attributes->GetItemByIndex(i, g, v); }
    STDMETHODIMP CopyAllItems(IMFAttributes* d) { return m_attributes->CopyAllItems(d); }
};

struct MediaStream : winrt::implements<MediaStream, CBaseAttributes<IMFAttributes>, IMFMediaStream, IMFMediaEventGenerator, IKsControl>
{
public:
    STDMETHOD(GetMediaSource)(_Outptr_ IMFMediaSource** ppMediaSource);
    STDMETHOD(GetStreamDescriptor)(_Outptr_ IMFStreamDescriptor** ppStreamDescriptor);
    STDMETHOD(RequestSample)(_In_opt_ IUnknown* pToken);
    STDMETHOD(BeginGetEvent)(_In_ IMFAsyncCallback* pCallback, _In_opt_ IUnknown* punkState);
    STDMETHOD(EndGetEvent)(_In_ IMFAsyncResult* pResult, _Outptr_ IMFMediaEvent** ppEvent);
    STDMETHOD(GetEvent)(DWORD dwFlags, _Outptr_ IMFMediaEvent** ppEvent);
    STDMETHOD(QueueEvent)(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, _In_opt_ const PROPVARIANT* pvValue);
    STDMETHOD_(NTSTATUS, KsProperty)(_In_ PKSPROPERTY, ULONG, _Inout_ LPVOID, ULONG, _Out_ ULONG*);
    STDMETHOD_(NTSTATUS, KsMethod)(_In_ PKSMETHOD, ULONG, _Inout_ LPVOID, ULONG, _Out_ ULONG*);
    STDMETHOD_(NTSTATUS, KsEvent)(_In_opt_ PKSEVENT, ULONG, _Inout_ LPVOID, ULONG, _Out_opt_ ULONG*);
    
    HRESULT Initialize(MediaSource* source);
    HRESULT SetAllocator(IUnknown* allocator);
    HRESULT SetD3DManager(IUnknown* manager);
    HRESULT Start(IMFMediaType* type);
    HRESULT Stop();
    void Shutdown();
    MFSampleAllocatorUsage GetAllocatorUsage() { return MFSampleAllocatorUsage_UsesProvidedAllocator; }
    FrameGenerator* GetFrameGenerator() { return m_frameGenerator.get(); }

private:
    winrt::slim_mutex m_lock;
    MF_STREAM_STATE m_state = MF_STREAM_STATE_STOPPED;
    GUID m_format = GUID_NULL;
    wil::com_ptr_nothrow<MediaSource> m_source;
    wil::com_ptr_nothrow<IMFStreamDescriptor> m_descriptor;
    wil::com_ptr_nothrow<IMFMediaEventQueue> m_queue;
    wil::com_ptr_nothrow<IMFVideoSampleAllocatorEx> m_allocator;
    std::unique_ptr<FrameGenerator> m_frameGenerator;
};

struct MediaSource : winrt::implements<MediaSource, CBaseAttributes<IMFAttributes>, IMFMediaSourceEx, IMFGetService, IKsControl, IMFSampleAllocatorControl, IDirectPortVirtuaCamControl>
{
public:
    STDMETHOD(BeginGetEvent)(_In_ IMFAsyncCallback* pCallback, _In_opt_ IUnknown* punkState);
    STDMETHOD(EndGetEvent)(_In_ IMFAsyncResult* pResult, _Outptr_ IMFMediaEvent** ppEvent);
    STDMETHOD(GetEvent)(DWORD dwFlags, _Outptr_ IMFMediaEvent** ppEvent);
    STDMETHOD(QueueEvent)(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, _In_opt_ const PROPVARIANT* pvValue);
    STDMETHOD(CreatePresentationDescriptor)(_Outptr_ IMFPresentationDescriptor** ppPresentationDescriptor);
    STDMETHOD(GetCharacteristics)(_Out_ DWORD* pdwCharacteristics);
    STDMETHOD(Pause)();
    STDMETHOD(Shutdown)();
    STDMETHOD(Start)(_In_ IMFPresentationDescriptor* pPresentationDescriptor, _In_opt_ const GUID* pguidTimeFormat, _In_opt_ const PROPVARIANT* pvarStartPosition);
    STDMETHOD(Stop)();
    STDMETHOD(GetSourceAttributes)(_Outptr_ IMFAttributes** ppAttributes);
    STDMETHOD(GetStreamAttributes)(DWORD dwStreamIdentifier, _Outptr_ IMFAttributes** ppAttributes);
    STDMETHOD(SetD3DManager)(_In_opt_ IUnknown* pManager);
    STDMETHOD(SetDefaultAllocator)(DWORD dwOutputStreamID, IUnknown* pAllocator);
    STDMETHOD(GetAllocatorUsage)(DWORD dwOutputStreamID, DWORD* pdwInputStreamID, MFSampleAllocatorUsage* peUsage);
    STDMETHOD(GetService)(_In_ REFGUID guidService, _In_ REFIID riid, _Out_ LPVOID* ppvObject);
    STDMETHOD_(NTSTATUS, KsProperty)(_In_ PKSPROPERTY, ULONG, _Inout_ LPVOID, ULONG, _Out_ ULONG*);
    STDMETHOD_(NTSTATUS, KsMethod)(_In_ PKSMETHOD, ULONG, _Inout_ LPVOID, ULONG, _Out_ ULONG*);
    STDMETHOD_(NTSTATUS, KsEvent)(_In_opt_ PKSEVENT, ULONG, _Inout_ LPVOID, ULONG, _Out_opt_ ULONG*);
    STDMETHOD(TogglePreviewWindow)();
    STDMETHOD(IsPreviewWindowVisible)(BOOL* pIsVisible);
    
    HRESULT Initialize(IMFAttributes* attributes);
    MediaStream* GetStream() { return m_stream.get(); }

private:
    winrt::slim_mutex m_lock;
    wil::com_ptr_nothrow<MediaStream> m_stream;
    wil::com_ptr_nothrow<IMFMediaEventQueue> m_queue;
    wil::com_ptr_nothrow<IMFPresentationDescriptor> m_descriptor;
    std::unique_ptr<PreviewWindow> m_previewWindow;
};

struct Activator : winrt::implements<Activator, CBaseAttributes<IMFActivate>>
{
public:
    STDMETHOD(ActivateObject)(REFIID riid, void** ppv);
    STDMETHOD(ShutdownObject)();
    STDMETHOD(DetachObject)();
    HRESULT Initialize();
private:
    winrt::com_ptr<MediaSource> m_source;
};

HRESULT Activator::Initialize()
{
    m_source = winrt::make_self<MediaSource>();
    RETURN_IF_FAILED(SetUINT32(MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES, 1));
    RETURN_IF_FAILED(SetGUID(MFT_TRANSFORM_CLSID_Attribute, CLSID_DirectPortVirtualCamera));
    RETURN_IF_FAILED(m_source->Initialize(this));
    return S_OK;
}

STDMETHODIMP Activator::ActivateObject(REFIID riid, void** ppv)
{
    RETURN_HR_IF_NULL(E_POINTER, ppv);
    *ppv = nullptr;
    RETURN_IF_FAILED_MSG(m_source->QueryInterface(riid, ppv), "Activator::ActivateObject QI failed");
    return S_OK;
}

STDMETHODIMP Activator::ShutdownObject() { return S_OK; }
STDMETHODIMP Activator::DetachObject() { m_source = nullptr; return S_OK; }

HRESULT MediaStream::Initialize(MediaSource* source)
{
    RETURN_HR_IF_NULL(E_POINTER, source);
    m_source.copy_from(source);
    m_frameGenerator = std::make_unique<FrameGenerator>();

    RETURN_IF_FAILED(SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
    RETURN_IF_FAILED(SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0));
    RETURN_IF_FAILED(SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
    RETURN_IF_FAILED(SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes::MFFrameSourceTypes_Color));
    RETURN_IF_FAILED(MFCreateEventQueue(&m_queue));

    wil::com_ptr_nothrow<IMFMediaType> rgbType;
    RETURN_IF_FAILED(MFCreateMediaType(&rgbType));
    RETURN_IF_FAILED(rgbType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    RETURN_IF_FAILED(rgbType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32));
    RETURN_IF_FAILED(rgbType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    RETURN_IF_FAILED(rgbType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
    RETURN_IF_FAILED(MFSetAttributeSize(rgbType.get(), MF_MT_FRAME_SIZE, VCAM_WIDTH, VCAM_HEIGHT));
    RETURN_IF_FAILED(MFSetAttributeRatio(rgbType.get(), MF_MT_FRAME_RATE, VCAM_FRAME_RATE_NUM, VCAM_FRAME_RATE_DEN));
    RETURN_IF_FAILED(MFSetAttributeRatio(rgbType.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

    wil::com_ptr_nothrow<IMFMediaType> nv12Type;
    RETURN_IF_FAILED(MFCreateMediaType(&nv12Type));
    RETURN_IF_FAILED(nv12Type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    RETURN_IF_FAILED(nv12Type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
    RETURN_IF_FAILED(nv12Type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    RETURN_IF_FAILED(nv12Type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
    RETURN_IF_FAILED(MFSetAttributeSize(nv12Type.get(), MF_MT_FRAME_SIZE, VCAM_WIDTH, VCAM_HEIGHT));
    RETURN_IF_FAILED(MFSetAttributeRatio(nv12Type.get(), MF_MT_FRAME_RATE, VCAM_FRAME_RATE_NUM, VCAM_FRAME_RATE_DEN));
    RETURN_IF_FAILED(MFSetAttributeRatio(nv12Type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

    IMFMediaType* availableMediaTypes[] = { nv12Type.get(), rgbType.get() };
    RETURN_IF_FAILED(MFCreateStreamDescriptor(0, ARRAYSIZE(availableMediaTypes), availableMediaTypes, &m_descriptor));

    wil::com_ptr_nothrow<IMFMediaTypeHandler> handler;
    RETURN_IF_FAILED(m_descriptor->GetMediaTypeHandler(&handler));
    RETURN_IF_FAILED(handler->SetCurrentMediaType(availableMediaTypes[0]));

    return S_OK;
}

HRESULT MediaStream::Start(IMFMediaType* type)
{
    winrt::slim_lock_guard lock(m_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_queue || !m_allocator);
    if (m_state == MF_STREAM_STATE_RUNNING) return S_OK;
    if (type)
    {
        RETURN_IF_FAILED(type->GetGUID(MF_MT_SUBTYPE, &m_format));
    }
    
    m_frameGenerator->Start();
    RETURN_IF_FAILED(m_allocator->InitializeSampleAllocator(10, type));
    RETURN_IF_FAILED(m_queue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr));
    m_state = MF_STREAM_STATE_RUNNING;
    return S_OK;
}

HRESULT MediaStream::Stop()
{
    winrt::slim_lock_guard lock(m_lock);
    if (m_state == MF_STREAM_STATE_STOPPED) return S_OK;
    m_state = MF_STREAM_STATE_STOPPED;
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_queue);
    
    m_frameGenerator->Stop();

    if(m_allocator) (void)m_allocator->UninitializeSampleAllocator();
    (void)m_queue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
    return S_OK;
}

void MediaStream::Shutdown()
{
    winrt::slim_lock_guard lock(m_lock);
    (void)Stop();
    if (m_queue) (void)m_queue->Shutdown();
    m_frameGenerator.reset();
    m_queue = nullptr;
    m_descriptor = nullptr;
    m_source = nullptr;
    m_allocator = nullptr;
}

STDMETHODIMP MediaStream::GetMediaSource(_Outptr_ IMFMediaSource** ppMediaSource)
{
    winrt::slim_lock_guard lock(m_lock);
    RETURN_HR_IF_NULL(E_POINTER, ppMediaSource);
    *ppMediaSource = nullptr;
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_source);
    m_source.copy_to(ppMediaSource);
    return S_OK;
}

STDMETHODIMP MediaStream::GetStreamDescriptor(_Outptr_ IMFStreamDescriptor** ppStreamDescriptor)
{
    winrt::slim_lock_guard lock(m_lock);
    RETURN_HR_IF_NULL(E_POINTER, ppStreamDescriptor);
    *ppStreamDescriptor = nullptr;
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_descriptor);
    m_descriptor.copy_to(ppStreamDescriptor);
    return S_OK;
}

STDMETHODIMP MediaStream::RequestSample(_In_opt_ IUnknown* pToken)
{
    winrt::slim_lock_guard lock(m_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_allocator || !m_queue || !m_frameGenerator);
    if (m_state != MF_STREAM_STATE_RUNNING) return MF_E_MEDIA_SOURCE_WRONGSTATE;

    wil::com_ptr_nothrow<IMFSample> sample;
    RETURN_IF_FAILED(m_allocator->AllocateSample(&sample));
    RETURN_IF_FAILED(sample->SetSampleTime(MFGetSystemTime()));
    RETURN_IF_FAILED(sample->SetSampleDuration(10000000 / VCAM_FRAME_RATE_NUM));

    wil::com_ptr_nothrow<IMFSample> outSample;
    RETURN_IF_FAILED(m_frameGenerator->Generate(sample.get(), m_format, &outSample));

    if (pToken)
    {
        RETURN_IF_FAILED(outSample->SetUnknown(MFSampleExtension_Token, pToken));
    }
    RETURN_IF_FAILED(m_queue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, outSample.get()));
    return S_OK;
}

HRESULT MediaStream::SetD3DManager(IUnknown* manager)
{
    winrt::slim_lock_guard lock(m_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_frameGenerator);
    return m_frameGenerator->SetD3DManager(manager);
}

STDMETHODIMP MediaStream::BeginGetEvent(_In_ IMFAsyncCallback* c, _In_opt_ IUnknown* s) { winrt::slim_lock_guard lock(m_lock); RETURN_HR_IF(MF_E_SHUTDOWN, !m_queue); return m_queue->BeginGetEvent(c, s); }
STDMETHODIMP MediaStream::EndGetEvent(_In_ IMFAsyncResult* r, _Outptr_ IMFMediaEvent** e) { winrt::slim_lock_guard lock(m_lock); RETURN_HR_IF_NULL(E_POINTER, e); *e = nullptr; RETURN_HR_IF(MF_E_SHUTDOWN, !m_queue); return m_queue->EndGetEvent(r, e); }
STDMETHODIMP MediaStream::GetEvent(DWORD f, _Outptr_ IMFMediaEvent** e) { winrt::slim_lock_guard lock(m_lock); RETURN_HR_IF_NULL(E_POINTER, e); *e = nullptr; RETURN_HR_IF(MF_E_SHUTDOWN, !m_queue); return m_queue->GetEvent(f, e); }
STDMETHODIMP MediaStream::QueueEvent(MediaEventType t, REFGUID g, HRESULT h, _In_opt_ const PROPVARIANT* v) { winrt::slim_lock_guard lock(m_lock); RETURN_HR_IF(MF_E_SHUTDOWN, !m_queue); return m_queue->QueueEventParamVar(t, g, h, v); }
STDMETHODIMP_(NTSTATUS) MediaStream::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) { return STATUS_NOT_IMPLEMENTED; }
STDMETHODIMP_(NTSTATUS) MediaStream::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) { return STATUS_NOT_IMPLEMENTED; }
STDMETHODIMP_(NTSTATUS) MediaStream::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) { return STATUS_NOT_IMPLEMENTED; }
HRESULT MediaStream::SetAllocator(IUnknown* allocator) { RETURN_HR_IF_NULL(E_POINTER, allocator); return allocator->QueryInterface(m_allocator.put()); }

HRESULT MediaSource::Initialize(IMFAttributes* attributes)
{
    if (attributes) { RETURN_IF_FAILED(attributes->CopyAllItems(this)); }

    wil::com_ptr_nothrow<IMFSensorProfileCollection> profileCollection;
    RETURN_IF_FAILED(MFCreateSensorProfileCollection(&profileCollection));
    wil::com_ptr_nothrow<IMFSensorProfile> legacyProfile;
    RETURN_IF_FAILED(MFCreateSensorProfile(KSCAMERAPROFILE_Legacy, 0, nullptr, &legacyProfile));
    RETURN_IF_FAILED(legacyProfile->AddProfileFilter(0, L"((RES==;FRT<=30,1;SUT==))"));
    RETURN_IF_FAILED(profileCollection->AddProfile(legacyProfile.get()));
    RETURN_IF_FAILED(SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, profileCollection.get()));

    m_stream = winrt::make_self<MediaStream>();
    RETURN_IF_FAILED(m_stream->Initialize(this));
    wil::com_ptr_nothrow<IMFStreamDescriptor> streamDesc;
    RETURN_IF_FAILED(m_stream->GetStreamDescriptor(&streamDesc));
    IMFStreamDescriptor* descs[] = { streamDesc.get() };
    RETURN_IF_FAILED(MFCreatePresentationDescriptor(ARRAYSIZE(descs), descs, &m_descriptor));
    RETURN_IF_FAILED(MFCreateEventQueue(&m_queue));
    m_previewWindow = std::make_unique<PreviewWindow>();

    return S_OK;
}

STDMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback* c, IUnknown* s) { winrt::slim_lock_guard lock(m_lock); RETURN_HR_IF(MF_E_SHUTDOWN, !m_queue); return m_queue->BeginGetEvent(c, s); }
STDMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) { winrt::slim_lock_guard lock(m_lock); RETURN_HR_IF_NULL(E_POINTER, e); *e = nullptr; RETURN_HR_IF(MF_E_SHUTDOWN, !m_queue); return m_queue->EndGetEvent(r, e); }
STDMETHODIMP MediaSource::GetEvent(DWORD f, IMFMediaEvent** e) { winrt::slim_lock_guard lock(m_lock); RETURN_HR_IF_NULL(E_POINTER, e); *e = nullptr; RETURN_HR_IF(MF_E_SHUTDOWN, !m_queue); return m_queue->GetEvent(f, e); }
STDMETHODIMP MediaSource::QueueEvent(MediaEventType t, REFGUID g, HRESULT h, const PROPVARIANT* v) { winrt::slim_lock_guard lock(m_lock); RETURN_HR_IF(MF_E_SHUTDOWN, !m_queue); return m_queue->QueueEventParamVar(t, g, h, v); }
STDMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** pd) { winrt::slim_lock_guard lock(m_lock); RETURN_HR_IF_NULL(E_POINTER, pd); *pd = nullptr; RETURN_HR_IF(MF_E_SHUTDOWN, !m_descriptor); return m_descriptor->Clone(pd); }
STDMETHODIMP MediaSource::GetCharacteristics(DWORD* c) { RETURN_HR_IF_NULL(E_POINTER, c); *c = MFMEDIASOURCE_IS_LIVE; return S_OK; }
STDMETHODIMP MediaSource::Pause() { return MF_E_INVALID_STATE_TRANSITION; }

STDMETHODIMP MediaSource::Shutdown()
{
    winrt::slim_lock_guard lock(m_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_queue);
    
    if (m_previewWindow) m_previewWindow->Stop();
    m_previewWindow.reset();
    
    if (m_stream) m_stream->Shutdown();
    m_stream = nullptr;
    if (m_queue) (void)m_queue->Shutdown();
    m_queue = nullptr;
    m_descriptor = nullptr;
    return S_OK;
}

STDMETHODIMP MediaSource::Start(_In_ IMFPresentationDescriptor* pd, const GUID* tf, const PROPVARIANT* pos)
{
    winrt::slim_lock_guard lock(m_lock);
    RETURN_HR_IF_NULL(E_POINTER, pd);
    RETURN_HR_IF_NULL(E_POINTER, pos);
    RETURN_HR_IF_MSG(E_INVALIDARG, tf && *tf != GUID_NULL, "Unsupported time format");
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_queue || !m_descriptor);

    DWORD count;
    RETURN_IF_FAILED(pd->GetStreamDescriptorCount(&count));
    if (count == 0) return MF_E_UNEXPECTED;

    BOOL selected = FALSE;
    wil::com_ptr_nothrow<IMFStreamDescriptor> desc;
    RETURN_IF_FAILED(pd->GetStreamDescriptorByIndex(0, &selected, &desc));

    if (selected)
    {
        RETURN_IF_FAILED(m_descriptor->SelectStream(0));
        wil::com_ptr_nothrow<IMFMediaTypeHandler> handler;
        wil::com_ptr_nothrow<IMFMediaType> type;
        RETURN_IF_FAILED(desc->GetMediaTypeHandler(&handler));
        RETURN_IF_FAILED(handler->GetCurrentMediaType(&type));
        RETURN_IF_FAILED(m_stream->Start(type.get()));
    }
    else 
    { 
        RETURN_IF_FAILED(m_descriptor->DeselectStream(0));
        RETURN_IF_FAILED(m_stream->Stop()); 
    }

    wil::unique_prop_variant time;
    RETURN_IF_FAILED(InitPropVariantFromInt64(MFGetSystemTime(), &time));
    return m_queue->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &time);
}

STDMETHODIMP MediaSource::Stop()
{
    winrt::slim_lock_guard lock(m_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_queue || !m_descriptor);
    RETURN_IF_FAILED(m_stream->Stop());
    RETURN_IF_FAILED(m_descriptor->DeselectStream(0));
    wil::unique_prop_variant time;
    RETURN_IF_FAILED(InitPropVariantFromInt64(MFGetSystemTime(), &time));
    return m_queue->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, &time);
}

STDMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes** a) { return QueryInterface(IID_PPV_ARGS(a)); }
STDMETHODIMP MediaSource::GetStreamAttributes(DWORD id, IMFAttributes** a) { if (id == 0 && m_stream) { return m_stream.copy_to(a); } return MF_E_INVALIDSTREAMNUMBER; }
STDMETHODIMP MediaSource::SetD3DManager(IUnknown* m) { RETURN_HR_IF_NULL(E_POINTER, m); winrt::slim_lock_guard lock(m_lock); if (m_stream) { return m_stream->SetD3DManager(m); } return MF_E_SHUTDOWN; }
STDMETHODIMP MediaSource::SetDefaultAllocator(DWORD id, IUnknown* a) { if (id == 0 && m_stream) return m_stream->SetAllocator(a); return MF_E_INVALIDSTREAMNUMBER; }
STDMETHODIMP MediaSource::GetAllocatorUsage(DWORD id, DWORD* inId, MFSampleAllocatorUsage* u) { RETURN_HR_IF_NULL(E_POINTER, inId); RETURN_HR_IF_NULL(E_POINTER, u); if (id == 0 && m_stream) { *inId = id; *u = m_stream->GetAllocatorUsage(); return S_OK; } return MF_E_INVALIDSTREAMNUMBER; }
STDMETHODIMP MediaSource::GetService(_In_ REFGUID guidService, _In_ REFIID riid, _Out_ LPVOID* ppvObject) { if (guidService == MR_CAMERA_CONTROL_SERVICE && riid == IID_IDirectPortVirtuaCamControl) { return this->QueryInterface(riid, ppvObject); } return MF_E_UNSUPPORTED_SERVICE; }
STDMETHODIMP_(NTSTATUS) MediaSource::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) { return STATUS_NOT_IMPLEMENTED; }
STDMETHODIMP_(NTSTATUS) MediaSource::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) { return STATUS_NOT_IMPLEMENTED; }
STDMETHODIMP_(NTSTATUS) MediaSource::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) { return STATUS_NOT_IMPLEMENTED; }

STDMETHODIMP MediaSource::TogglePreviewWindow()
{
    winrt::slim_lock_guard lock(m_lock);
    if (!m_previewWindow) return E_UNEXPECTED;

    BOOL isVisible = FALSE;
    IsPreviewWindowVisible(&isVisible);

    if (isVisible)
    {
        m_previewWindow->Stop();
    }
    else
    {
        m_previewWindow->Start(this);
    }
    return S_OK;
}

STDMETHODIMP MediaSource::IsPreviewWindowVisible(BOOL* pIsVisible)
{
    RETURN_HR_IF_NULL(E_POINTER, pIsVisible);
    winrt::slim_lock_guard lock(m_lock);
    *pIsVisible = (m_previewWindow && m_previewWindow->m_hwnd != nullptr);
    return S_OK;
}

void FrameGenerator::Start()
{
    m_shutdownSignal = false;
    m_workerThread = std::thread(&FrameGenerator::WorkerThreadProc, this);
}

void FrameGenerator::Stop()
{
    m_shutdownSignal = true;
    if (m_workerThread.joinable())
    {
        m_workerThread.join();
    }
}

HRESULT FrameGenerator::SetD3DManager(IUnknown* manager)
{
    RETURN_HR_IF_NULL(E_POINTER, manager);
    m_dxgiManager.reset();
    m_device.reset();
    m_context.reset();
    if (m_deviceHandle)
    {
        m_dxgiManager->CloseDeviceHandle(m_deviceHandle.get());
        m_deviceHandle.reset();
    }
    RETURN_IF_FAILED(manager->QueryInterface(m_dxgiManager.put()));
    RETURN_IF_FAILED(m_dxgiManager->OpenDeviceHandle(&m_deviceHandle));
    RETURN_IF_FAILED(m_dxgiManager->GetVideoService(m_deviceHandle.get(), IID_PPV_ARGS(&m_device)));
    RETURN_IF_FAILED(m_device->QueryInterface(m_device5.put()));
    m_device->GetImmediateContext(m_context.put());
    RETURN_IF_FAILED(m_context->QueryInterface(m_context4.put()));

    wil::com_ptr<IDXGIDevice> dxgiDevice;
    RETURN_IF_FAILED(m_device.query_to(&dxgiDevice));
    wil::com_ptr<IDXGIAdapter> adapter;
    RETURN_IF_FAILED(dxgiDevice->GetAdapter(&adapter));
    DXGI_ADAPTER_DESC desc{};
    RETURN_IF_FAILED(adapter->GetDesc(&desc));
    m_adapterLuid = desc.AdapterLuid;

    return S_OK;
}

HRESULT FrameGenerator::EnsureResources()
{
    if (m_lastGoodFrame) return S_OK;
    RETURN_HR_IF(E_UNEXPECTED, !m_device);
    
    RETURN_IF_FAILED(CreateDeviceResources());
    RETURN_IF_FAILED(CreateNoSignalResources());
    
    return S_OK;
}

HRESULT FrameGenerator::CreateDeviceResources()
{
    CD3D11_TEXTURE2D_DESC desc(DXGI_FORMAT_B8G8R8A8_UNORM, VCAM_WIDTH, VCAM_HEIGHT, 1, 1, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    RETURN_IF_FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_lastGoodFrame));

    RETURN_IF_FAILED(CoCreateInstance(CLSID_VideoProcessorMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_converter)));
    wil::com_ptr_nothrow<IMFMediaType> inType, outType;
    RETURN_IF_FAILED(MFCreateMediaType(&inType));
    RETURN_IF_FAILED(inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    RETURN_IF_FAILED(inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32));
    RETURN_IF_FAILED(MFSetAttributeSize(inType.get(), MF_MT_FRAME_SIZE, VCAM_WIDTH, VCAM_HEIGHT));
    RETURN_IF_FAILED(m_converter->SetInputType(0, inType.get(), 0));

    RETURN_IF_FAILED(MFCreateMediaType(&outType));
    RETURN_IF_FAILED(outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    RETURN_IF_FAILED(outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
    RETURN_IF_FAILED(MFSetAttributeSize(outType.get(), MF_MT_FRAME_SIZE, VCAM_WIDTH, VCAM_HEIGHT));
    RETURN_IF_FAILED(m_converter->SetOutputType(0, outType.get(), 0));
    RETURN_IF_FAILED(m_converter->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(m_dxgiManager.get())));

    return S_OK;
}

HRESULT FrameGenerator::CreateNoSignalResources()
{
    wil::com_ptr_nothrow<IDXGISurface> surface;
    RETURN_IF_FAILED(m_lastGoodFrame.query_to(&surface));

    wil::com_ptr_nothrow<ID2D1Factory> d2d1Factory;
    RETURN_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, IID_PPV_ARGS(&d2d1Factory)));
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED));
    RETURN_IF_FAILED(d2d1Factory->CreateDxgiSurfaceRenderTarget(surface.get(), props, &m_renderTarget));
    
    RETURN_IF_FAILED(m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White, 1.0f), &m_textBrush));

    RETURN_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(m_dwriteFactory.put())));
    RETURN_IF_FAILED(m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 60.f, L"en-us", &m_textFormat));
    RETURN_IF_FAILED(m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER));
    RETURN_IF_FAILED(m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER));

    return S_OK;
}

HRESULT FrameGenerator::RenderNoSignal()
{
    if (!m_renderTarget) return E_UNEXPECTED;
    
    m_renderTarget->BeginDraw();
    m_renderTarget->Clear(D2D1::ColorF(0.0f, 0.0f, 0.5f, 1.0f));
    
    const wchar_t text[] = L"NO SIGNAL";
    D2D1_RECT_F layoutRect = D2D1::RectF(0.f, (float)VCAM_HEIGHT/2.0f - 100.f, (float)VCAM_WIDTH, (float)VCAM_HEIGHT/2.0f + 100.f);
    m_renderTarget->DrawTextW(text, (UINT32)wcslen(text), m_textFormat.get(), layoutRect, m_textBrush.get());

    return m_renderTarget->EndDraw();
}

HRESULT FrameGenerator::ConvertRGBToNV12(IMFSample* inSample, IMFSample** outSample)
{
    RETURN_HR_IF(E_UNEXPECTED, !m_converter);
    RETURN_IF_FAILED(m_converter->ProcessInput(0, inSample, 0));
    
    MFT_OUTPUT_DATA_BUFFER buffer = {};
    buffer.dwStreamID = 0;
    
    wil::com_ptr_nothrow<IMFSample> outSampleInternal;
    DWORD status = 0;
    
    HRESULT hr = m_converter->ProcessOutput(0, 1, &buffer, &status);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK; 
    RETURN_IF_FAILED(hr);

    *outSample = buffer.pSample;
    if (*outSample) (*outSample)->AddRef();
    
    wil::com_ptr<IMFSample>(buffer.pSample).reset();
    wil::com_ptr<IUnknown>(buffer.pEvents).reset();

    return S_OK;
}

HRESULT FrameGenerator::Generate(IMFSample* sample, REFGUID format, IMFSample** outSample)
{
    RETURN_HR_IF_NULL(E_POINTER, sample);
    RETURN_HR_IF_NULL(E_POINTER, outSample);
    *outSample = nullptr;

    if (!m_device) return E_UNEXPECTED;
    RETURN_IF_FAILED(EnsureResources());

    wil::com_ptr_nothrow<ID3D11Texture2D> outputTexture;
    {
        CD3D11_TEXTURE2D_DESC desc;
        m_lastGoodFrame->GetDesc(&desc);
        RETURN_IF_FAILED(m_device->CreateTexture2D(&desc, nullptr, &outputTexture));
        winrt::slim_lock_guard lock(m_producerLock);
        m_context->CopyResource(outputTexture.get(), m_lastGoodFrame.get());
    }
    
    RETURN_IF_FAILED(sample->RemoveAllBuffers());
    wil::com_ptr_nothrow<IMFMediaBuffer> mediaBuffer;
    RETURN_IF_FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), outputTexture.get(), 0, FALSE, &mediaBuffer));
    RETURN_IF_FAILED(sample->AddBuffer(mediaBuffer.get()));
    
    if (format == MFVideoFormat_NV12)
    {
        RETURN_IF_FAILED(ConvertRGBToNV12(sample, outSample));
    }
    else
    {
        sample->AddRef();
        *outSample = sample;
    }

    return S_OK;
}

void FrameGenerator::WorkerThreadProc()
{
    (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    
    wil::com_ptr_nothrow<ID3D11Texture2D> privateTexture;
    
    while (!m_shutdownSignal)
    {
        FindAndConnectToProducer();
        
        bool isConnectedNow = false;
        {
            winrt::slim_lock_guard lock(m_producerLock);
            isConnectedNow = m_producer.isConnected;
        }

        if (isConnectedNow)
        {
            UINT64 latestFrame = 0;
            UINT64 lastSeen = 0;
            wil::com_ptr_nothrow<ID3D11Fence> fence;
            
            {
                winrt::slim_lock_guard lock(m_producerLock);
                if (!m_producer.isConnected) continue;
                latestFrame = m_producer.pManifestView->frameValue;
                lastSeen = m_producer.lastSeenFrame;
                fence = m_producer.sharedFence;
            }

            if (latestFrame > lastSeen)
            {
                if (SUCCEEDED(m_context4->Wait(fence.get(), latestFrame)))
                {
                    winrt::slim_lock_guard lock(m_producerLock);
                    if (!m_producer.isConnected) continue;
                    
                    if (!privateTexture)
                    {
                        D3D11_TEXTURE2D_DESC desc;
                        m_producer.sharedTexture->GetDesc(&desc);
                        m_device->CreateTexture2D(&desc, nullptr, &privateTexture);
                    }
                    if (privateTexture)
                    {
                       m_context->CopyResource(privateTexture.get(), m_producer.sharedTexture.get());
                       m_context->CopyResource(m_lastGoodFrame.get(), privateTexture.get());
                    }

                    m_producer.lastSeenFrame = latestFrame;
                }
            }
             std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        else
        {
            {
                winrt::slim_lock_guard lock(m_producerLock);
                if (SUCCEEDED(EnsureResources()))
                {
                    (void)RenderNoSignal();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    CoUninitialize();
}

void FrameGenerator::FindAndConnectToProducer()
{
    {
        winrt::slim_lock_guard lock(m_producerLock);
        if (m_producer.isConnected)
        {
            wil::unique_handle hProcess(OpenProcess(SYNCHRONIZE, FALSE, m_producer.producerPid));
            if (!hProcess || WaitForSingleObject(hProcess.get(), 0) != WAIT_TIMEOUT)
            {
                DisconnectFromProducer();
            }
            return;
        }
    }

    wil::unique_handle hSnapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!hSnapshot) return;

    PROCESSENTRY32W pe32 = { sizeof(pe32) };
    if (!Process32FirstW(hSnapshot.get(), &pe32)) return;

    do
    {
        std::wstring manifestName = PRODUCER_MANIFEST_PREFIX + std::to_wstring(pe32.th32ProcessID);
        wil::unique_handle hManifest(OpenFileMappingW(FILE_MAP_READ, FALSE, manifestName.c_str()));
        if (!hManifest) continue;

        wil::unique_mapview_ptr<BroadcastManifest> pManifestView(
            (BroadcastManifest*)MapViewOfFile(hManifest.get(), FILE_MAP_READ, 0, 0, sizeof(BroadcastManifest))
        );
        if (!pManifestView) continue;
        if (memcmp(&pManifestView->adapterLuid, &m_adapterLuid, sizeof(LUID)) != 0) continue;

        wil::com_ptr_nothrow<ID3D11Fence> tempFence;
        wil::unique_handle hFence;
        if (FAILED(GetHandleFromName(pManifestView->fenceName, &hFence)) || !hFence || FAILED(m_device5->OpenSharedFence(hFence.get(), IID_PPV_ARGS(&tempFence)))) continue;
        
        wil::com_ptr_nothrow<ID3D11Texture2D> tempTexture;
        wil::unique_handle hTexture;
        if (FAILED(GetHandleFromName(pManifestView->textureName, &hTexture)) || !hTexture || FAILED(m_device->OpenSharedResource(hTexture.get(), IID_PPV_ARGS(&tempTexture)))) continue;
        
        {
            winrt::slim_lock_guard lock(m_producerLock);
            m_producer.isConnected = true;
            m_producer.producerPid = pe32.th32ProcessID;
            m_producer.hManifest = std::move(hManifest);
            m_producer.pManifestView = std::move(pManifestView);
            m_producer.sharedFence = std::move(tempFence);
            m_producer.sharedTexture = std::move(tempTexture);
        }
        return;
        
    } while (Process32NextW(hSnapshot.get(), &pe32));
}

void FrameGenerator::DisconnectFromProducer()
{
    m_producer.isConnected = false;
    m_producer.producerPid = 0;
    m_producer.hManifest.reset();
    m_producer.pManifestView.reset();
    m_producer.sharedTexture.reset();
    m_producer.sharedFence.reset();
    m_producer.lastSeenFrame = 0;
}

HRESULT FrameGenerator::GetHandleFromName(const WCHAR* name, HANDLE* handle)
{
    wil::com_ptr<ID3D12Device> d3d12Device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12Device)))) return E_FAIL;
    return d3d12Device->OpenSharedHandleByName(name, GENERIC_ALL, handle);
}

void PreviewWindow::Start(MediaSource* mediaSource)
{
    m_mediaSource = mediaSource;
    m_shutdown = false;
    m_thread = std::thread(&PreviewWindow::ThreadProc, this);
}

void PreviewWindow::Stop()
{
    if (m_hwnd)
    {
        PostMessage(m_hwnd, WM_CLOSE, 0, 0);
    }
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

void PreviewWindow::ThreadProc()
{
    (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = PreviewWindow::WndProc;
    wc.hInstance = g_module;
    wc.lpszClassName = PREVIEW_WND_CLASS;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    m_hwnd = CreateWindowExW(0, PREVIEW_WND_CLASS, L"DirectPort VirtuaCam Preview", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 450, nullptr, nullptr, g_module, this);

    if (m_hwnd)
    {
        ShowWindow(m_hwnd, SW_SHOW);
        UpdateWindow(m_hwnd);

        MSG msg;
        while (GetMessage(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    m_hwnd = nullptr;
    UnregisterClassW(PREVIEW_WND_CLASS, g_module);
    CoUninitialize();
}

LRESULT CALLBACK PreviewWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    PreviewWindow* pThis = nullptr;
    if (msg == WM_NCCREATE)
    {
        CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
        pThis = reinterpret_cast<PreviewWindow*>(pCreate->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)pThis);
        pThis->m_hwnd = hwnd;
    }
    else
    {
        pThis = reinterpret_cast<PreviewWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (pThis)
    {
        switch (msg)
        {
        case WM_CREATE:
            if (FAILED(pThis->InitD3D())) return -1;
            SetTimer(hwnd, 1, 16, nullptr);
            break;
        case WM_TIMER:
            pThis->RenderFrame();
            break;
        case WM_PAINT:
            pThis->RenderFrame();
            ValidateRect(hwnd, NULL);
            break;
        case WM_SIZE:
             if (pThis->m_swapChain && wParam != SIZE_MINIMIZED)
             {
                 pThis->m_rtv.reset();
                 pThis->m_swapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                 wil::com_ptr<ID3D11Texture2D> backBuffer;
                 pThis->m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
                 pThis->m_device->CreateRenderTargetView(backBuffer.get(), nullptr, &pThis->m_rtv);
             }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            break;
        case WM_DESTROY:
            pThis->CleanupD3D();
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }
    }
    return 0;
}

HRESULT PreviewWindow::InitD3D()
{
    m_device = m_mediaSource->GetStream()->GetFrameGenerator()->GetDevice();
    RETURN_HR_IF_NULL(E_UNEXPECTED, m_device);
    m_device->GetImmediateContext(m_context.put());

    RECT rc;
    GetClientRect(m_hwnd, &rc);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = rc.right - rc.left;
    sd.BufferDesc.Height = rc.bottom - rc.top;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = m_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    wil::com_ptr<IDXGIDevice> dxgiDevice;
    RETURN_IF_FAILED(m_device->QueryInterface(dxgiDevice.addressof()));
    wil::com_ptr<IDXGIAdapter> adapter;
    RETURN_IF_FAILED(dxgiDevice->GetAdapter(adapter.addressof()));
    wil::com_ptr<IDXGIFactory> factory;
    RETURN_IF_FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)));
    RETURN_IF_FAILED(factory->CreateSwapChain(m_device.get(), &sd, &m_swapChain));
    
    wil::com_ptr<ID3D11Texture2D> backBuffer;
    RETURN_IF_FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)));
    RETURN_IF_FAILED(m_device->CreateRenderTargetView(backBuffer.get(), nullptr, &m_rtv));

    const char* vsCode = "struct VOut{float4 pos:SV_POSITION;float2 uv:TEXCOORD;};VOut main(uint id:SV_VertexID){VOut o;o.uv=float2((id<<1)&2,id&2);o.pos=float4(o.uv.x*2-1,1-o.uv.y*2,0,1);return o;}";
    const char* psCode = "Texture2D t:register(t0);SamplerState s:register(s0);float4 main(float4 p:SV_POSITION,float2 u:TEXCOORD):SV_Target{return t.Sample(s,u);}";

    wil::com_ptr<ID3DBlob> vsBlob, psBlob, errBlob;
    D3DCompile(vsCode, strlen(vsCode), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &errBlob);
    RETURN_IF_FAILED(m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vertexShader));
    
    D3DCompile(psCode, strlen(psCode), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &errBlob);
    RETURN_IF_FAILED(m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_pixelShader));
    
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    RETURN_IF_FAILED(m_device->CreateSamplerState(&sampDesc, &m_sampler));

    return S_OK;
}

void PreviewWindow::RenderFrame()
{
    if (!m_rtv) return;
    
    wil::com_ptr<ID3D11Texture2D> frame = m_mediaSource->GetStream()->GetFrameGenerator()->GetLastFrame();
    
    const float clearColor[] = { 0.1f, 0.1f, 0.1f, 1.0f };
    m_context->ClearRenderTargetView(m_rtv.get(), clearColor);

    if (frame)
    {
        wil::com_ptr<ID3D11ShaderResourceView> srv;
        m_device->CreateShaderResourceView(frame.get(), nullptr, &srv);
        
        RECT rc; GetClientRect(m_hwnd, &rc);
        D3D11_VIEWPORT vp = { 0, 0, (float)(rc.right - rc.left), (float)(rc.bottom - rc.top), 0, 1 };
        m_context->RSSetViewports(1, &vp);
        m_context->OMSetRenderTargets(1, m_rtv.get_address_of(), nullptr);
        m_context->VSSetShader(m_vertexShader.get(), nullptr, 0);
        m_context->PSSetShader(m_pixelShader.get(), nullptr, 0);
        m_context->PSSetShaderResources(0, 1, srv.get_address_of());
        m_context->PSSetSamplers(0, 1, m_sampler.get_address_of());
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->Draw(3, 0);
    }
    
    m_swapChain->Present(1, 0);
}

void PreviewWindow::CleanupD3D()
{
    KillTimer(m_hwnd, 1);
    m_rtv.reset();
    m_sampler.reset();
    m_pixelShader.reset();
    m_vertexShader.reset();
    m_swapChain.reset();
    m_context.reset();
    m_device.reset();
}

struct ClassFactory : winrt::implements<ClassFactory, IClassFactory>
{
    STDMETHODIMP CreateInstance(IUnknown* outer, GUID const& riid, void** result) noexcept final
    {
        *result = nullptr;
        RETURN_HR_IF(CLASS_E_NOAGGREGATION, outer);
        auto vcam = winrt::make_self<Activator>();
        RETURN_IF_FAILED(vcam->Initialize());
        return vcam->QueryInterface(riid, result);
    }
    STDMETHODIMP LockServer(BOOL) noexcept final { return S_OK; }
};

extern "C"
{
    BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID)
    {
        if (dwReason == DLL_PROCESS_ATTACH)
        {
            g_module = hModule;
            DisableThreadLibraryCalls(hModule);
        }
        return TRUE;
    }

    _Check_return_ STDAPI DllGetClassObject(_In_ REFCLSID rclsid, _In_ REFIID riid, _Outptr_ LPVOID* ppv)
    {
        RETURN_HR_IF_NULL(E_POINTER, ppv);
        *ppv = nullptr;
        if (rclsid == CLSID_DirectPortVirtualCamera)
        {
            return winrt::make_self<ClassFactory>()->QueryInterface(riid, ppv);
        }
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    STDAPI DllCanUnloadNow()
    {
        return winrt::get_module_lock() ? S_FALSE : S_OK;
    }

    STDAPI DllRegisterServer()
    {
        try
        {
            auto modulePath = wil::GetModuleFileNameW(g_module);
            wchar_t clsidString[40];
            THROW_IF_FAILED(StringFromGUID2(CLSID_DirectPortVirtualCamera, clsidString, ARRAYSIZE(clsidString)));
            std::wstring keyPath = L"Software\\Classes\\CLSID\\" + std::wstring(clsidString);
            wil::unique_hkey key;
            THROW_IF_WIN32_ERROR(RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key, nullptr));
            THROW_IF_WIN32_ERROR(RegSetValueExW(key.get(), nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(VCAM_FRIENDLY_NAME), (wcslen(VCAM_FRIENDLY_NAME) + 1) * sizeof(wchar_t)));
            wil::unique_hkey inprocKey;
            THROW_IF_WIN32_ERROR(RegCreateKeyExW(key.get(), L"InprocServer32", 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &inprocKey, nullptr));
            THROW_IF_WIN32_ERROR(RegSetValueExW(inprocKey.get(), nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(modulePath.get()), (wcslen(modulePath.get()) + 1) * sizeof(wchar_t)));
            const wchar_t* threadingModel = L"Both";
            THROW_IF_WIN32_ERROR(RegSetValueExW(inprocKey.get(), L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE*>(threadingModel), (wcslen(threadingModel) + 1) * sizeof(wchar_t)));
            return S_OK;
        }
        CATCH_RETURN();
    }

    STDAPI DllUnregisterServer()
    {
        try
        {
            wchar_t clsidString[40];
            THROW_IF_FAILED(StringFromGUID2(CLSID_DirectPortVirtualCamera, clsidString, ARRAYSIZE(clsidString)));
            std::wstring keyPath = L"Software\\Classes\\CLSID\\" + std::wstring(clsidString);
            THROW_IF_WIN32_ERROR(RegDeleteTreeW(HKEY_LOCAL_MACHINE, keyPath.c_str()));
            return S_OK;
        }
        CATCH_RETURN();
    }
}