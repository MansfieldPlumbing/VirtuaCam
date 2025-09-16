/////////////////////////////////////////////////////////////////////////////////////////
//
// DirectPortVirtuaCam.cpp
//
// This file is the in-process COM server (DLL) that implements the media source for
// the DirectPort Virtual Camera. When an application uses the camera, this DLL is
// loaded into its process.
//
// It uses the DirectPort library to find and consume a texture stream from another
// DirectPort producer and delivers those frames to the camera application.
//
/////////////////////////////////////////////////////////////////////////////////////////

#include <sdkddkver.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>

// Media Foundation
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <ks.h>
#include <ksmedia.h>

// D3D11 for frame generation/copying
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <dwrite.h>

// WRL for COM helpers
#include <wrl/implements.h>
#include <wrl/module.h>
#include <wrl/client.h>

// WIL for error handling
#include <wil/com.h>
#include <wil/result.h>

// DirectPort library for consuming shared textures
#include "DirectPort.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "propsys.lib")

using namespace Microsoft::WRL;

// ---
// GUIDs and Constants
// ---

// NOTE: This CLSID must exactly match the one in DirectPortVirtuaCamManager.cpp
DEFINE_GUID(CLSID_DirectPortVirtualCamera, 0x55586753, 0x09a1, 0x4e89, {0xa2, 0x6a, 0x86, 0x75, 0x30, 0x9a, 0x44, 0x9b});

const WCHAR* VCAM_FRIENDLY_NAME = L"DirectPort Virtual Camera (Jenny)";
const UINT32 VIDEO_FPS = 30;
const LONGLONG VIDEO_FRAME_DURATION = 10000000 / VIDEO_FPS;

// ---
// Helper class to consume a DirectPort stream and generate fallback frames
// ---
class FrameProvider
{
    wil::com_ptr_nothrow<ID3D11Device> m_d3d11Device;
    wil::com_ptr_nothrow<ID3D11DeviceContext> m_d3d11Context;
    
    // For consuming a DirectPort stream
    std::shared_ptr<DirectPort::DeviceD3D11> m_dpDevice;
    std::shared_ptr<DirectPort::Consumer> m_dpConsumer;
    std::shared_ptr<DirectPort::Texture> m_dpTexture;

    // For generating a fallback "Searching..." frame
    wil::com_ptr_nothrow<ID2D1RenderTarget> m_renderTarget;
    wil::com_ptr_nothrow<ID2D1SolidColorBrush> m_brush;
    wil::com_ptr_nothrow<IDWriteTextFormat> m_textFormat;

public:
    FrameProvider() = default;

    HRESULT Initialize()
    {
        // Create a D3D11 device to handle texture copies and fallback rendering
        D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        RETURN_IF_FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION, &m_d3d11Device, nullptr, &m_d3d11Context));

        // Initialize the DirectPort D3D11 device wrapper
        m_dpDevice = DirectPort::DeviceD3D11::create();

        return S_OK;
    }

    void CheckForSource()
    {
        if (m_dpConsumer && m_dpConsumer->is_alive())
        {
            return; // Already connected and producer is running
        }

        m_dpConsumer.reset();
        m_dpTexture.reset();

        // Use DirectPort's discover function to find available producers
        auto producers = DirectPort::discover();
        if (!producers.empty())
        {
            try
            {
                // Connect to the first one found
                m_dpConsumer = m_dpDevice->connect_to_producer(producers[0].pid);
                if (m_dpConsumer)
                {
                    m_dpTexture = m_dpConsumer->get_texture();
                }
            }
            catch (...) { /* Connection failed, will generate fallback */ }
        }
    }

    HRESULT GenerateFrame(ID3D11Texture2D* targetTexture)
    {
        RETURN_HR_IF_NULL(E_POINTER, targetTexture);
        CheckForSource();

        if (m_dpConsumer && m_dpConsumer->wait_for_frame())
        {
            // If a producer is connected and has a new frame, copy it
            ID3D11Texture2D* sourceTexture = reinterpret_cast<ID3D11Texture2D*>(m_dpTexture->get_d3d11_texture_ptr());
            if (sourceTexture)
            {
                m_d3d11Context->CopyResource(targetTexture, sourceTexture);
                return S_OK;
            }
        }
        
        // If no producer or no new frame, generate a fallback image
        return GenerateFallbackFrame(targetTexture);
    }

private:
    HRESULT GenerateFallbackFrame(ID3D11Texture2D* targetTexture)
    {
        // Create a D2D render target on the target texture to draw the message
        D3D11_TEXTURE2D_DESC desc;
        targetTexture->GetDesc(&desc);

        wil::com_ptr_nothrow<IDXGISurface> dxgiSurface;
        RETURN_IF_FAILED(targetTexture->QueryInterface(&dxgiSurface));

        if (!m_renderTarget)
        {
            wil::com_ptr_nothrow<ID2D1Factory> d2dFactory;
            RETURN_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory));

            D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED));
            RETURN_IF_FAILED(d2dFactory->CreateDxgiSurfaceRenderTarget(dxgiSurface.get(), &props, &m_renderTarget));
            RETURN_IF_FAILED(m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_brush));

            wil::com_ptr_nothrow<IDWriteFactory> writeFactory;
            RETURN_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&writeFactory)));
            RETURN_IF_FAILED(writeFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 48.0f, L"en-us", &m_textFormat));
            m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        else
        {
            // If we already have a render target, ensure it's still valid for the surface
            wil::com_ptr_nothrow<ID2D1DxgiSurfaceRenderTarget> dxgiRT;
            if (SUCCEEDED(m_renderTarget->QueryInterface(&dxgiRT)))
            {
                dxgiRT->SetDxgiSurface(dxgiSurface.get());
            }
        }

        m_renderTarget->BeginDraw();
        m_renderTarget->Clear(D2D1::ColorF(D2D1::ColorF::DarkSlateGray));
        
        const WCHAR* message = L"DirectPort: Searching for producer...";
        D2D1_RECT_F layoutRect = D2D1::RectF(0.f, 0.f, (float)desc.Width, (float)desc.Height);
        m_renderTarget->DrawText(message, wcslen(message), m_textFormat.get(), &layoutRect, m_brush.get());

        RETURN_IF_FAILED(m_renderTarget->EndDraw());

        return S_OK;
    }
};

// ---
// Media Foundation Implementation (based on reference DLL)
// ---
namespace DirectPortVCam
{
    class MediaSource; 

    class MediaStream :
        public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMFMediaStream, IMFMediaEventGenerator>
    {
    public:
        IFACEMETHOD(BeginGetEvent)(IMFAsyncCallback* pCallback, IUnknown* punkState) { return m_spEventQueue->BeginGetEvent(pCallback, punkState); }
        IFACEMETHOD(EndGetEvent)(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) { return m_spEventQueue->EndGetEvent(pResult, ppEvent); }
        IFACEMETHOD(GetEvent)(DWORD dwFlags, IMFMediaEvent** ppEvent) { return m_spEventQueue->GetEvent(dwFlags, ppEvent); }
        IFACEMETHOD(QueueEvent)(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue) { return m_spEventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue); }
        
        IFACEMETHOD(GetMediaSource)(IMFMediaSource** ppMediaSource);
        IFACEMETHOD(GetStreamDescriptor)(IMFStreamDescriptor** ppStreamDescriptor);
        IFACEMETHOD(RequestSample)(IUnknown* pToken);

        MediaStream() : m_state(MF_STREAM_STATE_STOPPED), m_timeStamp(0) {}
        HRESULT RuntimeClassInitialize(MediaSource* pSource, UINT32 width, UINT32 height);
        HRESULT Start();
        HRESULT Stop();
        HRESULT Shutdown();

    private:
        std::mutex m_mutex;
        ComPtr<IMFMediaSource> m_spSource;
        ComPtr<IMFMediaEventQueue> m_spEventQueue;
        ComPtr<IMFStreamDescriptor> m_spStreamDescriptor;
        MF_STREAM_STATE m_state;
        LONGLONG m_timeStamp;
        FrameProvider m_frameProvider;
        UINT32 m_width;
        UINT32 m_height;
    };

    class MediaSource :
        public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMFMediaSource>
    {
    public:
        IFACEMETHOD(GetCharacteristics)(DWORD* pdwCharacteristics);
        IFACEMETHOD(CreatePresentationDescriptor)(IMFPresentationDescriptor** ppPresentationDescriptor);
        IFACEMETHOD(Start)(IMFPresentationDescriptor* pPresentationDescriptor, const GUID* pguidTimeFormat, const PROPVARIANT* pvarStartPosition);
        IFACEMETHOD(Stop)();
        IFACEMETHOD(Pause)();
        IFACEMETHOD(Shutdown)();
        IFACEMETHOD(BeginGetEvent)(IMFAsyncCallback* pCallback, IUnknown* punkState) { return m_spEventQueue->BeginGetEvent(pCallback, punkState); }
        IFACEMETHOD(EndGetEvent)(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) { return m_spEventQueue->EndGetEvent(pResult, ppEvent); }
        IFACEMETHOD(GetEvent)(DWORD dwFlags, IMFMediaEvent** ppEvent) { return m_spEventQueue->GetEvent(dwFlags, ppEvent); }
        IFACEMETHOD(QueueEvent)(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue) { return m_spEventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue); }
        
        MediaSource() = default;
        HRESULT RuntimeClassInitialize();

    private:
        std::mutex m_mutex;
        ComPtr<IMFMediaEventQueue> m_spEventQueue;
        ComPtr<IMFPresentationDescriptor> m_spPresentationDescriptor;
        ComPtr<MediaStream> m_spStream;
    };

    class Activator : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMFActivate>
    {
    public:
        Activator() { MFCreateAttributes(&m_spAttributes, 1); }
        IFACEMETHOD(ActivateObject)(REFIID riid, void** ppv) {
            if (!ppv) return E_POINTER;
            ComPtr<MediaSource> spSource;
            HRESULT hr = MakeAndInitialize<MediaSource>(&spSource);
            if (FAILED(hr)) return hr;
            return spSource->QueryInterface(riid, ppv);
        }
        IFACEMETHOD(ShutdownObject)() { m_spAttributes.Reset(); return S_OK; }
        IFACEMETHOD(DetachObject)() { m_spAttributes.Reset(); return S_OK; }
        // Pass-through IMFAttributes implementation
        IFACEMETHOD(GetItem)(REFGUID g, PROPVARIANT* v) { return m_spAttributes->GetItem(g, v); }
        IFACEMETHOD(GetItemType)(REFGUID g, MF_ATTRIBUTE_TYPE* t) { return m_spAttributes->GetItemType(g, t); }
        IFACEMETHOD(CompareItem)(REFGUID g, REFPROPVARIANT v, BOOL* r) { return m_spAttributes->CompareItem(g, v, r); }
        IFACEMETHOD(Compare)(IMFAttributes* t, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r) { return m_spAttributes->Compare(t, m, r); }
        IFACEMETHOD(GetUINT32)(REFGUID g, UINT32* v) { return m_spAttributes->GetUINT32(g, v); }
        IFACEMETHOD(GetUINT64)(REFGUID g, UINT64* v) { return m_spAttributes->GetUINT64(g, v); }
        IFACEMETHOD(GetDouble)(REFGUID g, double* v) { return m_spAttributes->GetDouble(g, v); }
        IFACEMETHOD(GetGUID)(REFGUID g, GUID* v) { return m_spAttributes->GetGUID(g, v); }
        IFACEMETHOD(GetStringLength)(REFGUID g, UINT32* l) { return m_spAttributes->GetStringLength(g, l); }
        IFACEMETHOD(GetString)(REFGUID g, LPWSTR v, UINT32 s, UINT32* l) { return m_spAttributes->GetString(g, v, s, l); }
        IFACEMETHOD(GetAllocatedString)(REFGUID g, LPWSTR* v, UINT32* l) { return m_spAttributes->GetAllocatedString(g, v, l); }
        IFACEMETHOD(GetBlobSize)(REFGUID g, UINT32* s) { return m_spAttributes->GetBlobSize(g, s); }
        IFACEMETHOD(GetBlob)(REFGUID g, UINT8* b, UINT32 s, UINT32* z) { return m_spAttributes->GetBlob(g, b, s, z); }
        IFACEMETHOD(GetAllocatedBlob)(REFGUID g, UINT8** b, UINT32* s) { return m_spAttributes->GetAllocatedBlob(g, b, s); }
        IFACEMETHOD(GetUnknown)(REFGUID g, REFIID i, LPVOID* v) { return m_spAttributes->GetUnknown(g, i, v); }
        IFACEMETHOD(SetItem)(REFGUID g, REFPROPVARIANT v) { return m_spAttributes->SetItem(g, v); }
        IFACEMETHOD(DeleteItem)(REFGUID g) { return m_spAttributes->DeleteItem(g); }
        IFACEMETHOD(DeleteAllItems)() { return m_spAttributes->DeleteAllItems(); }
        IFACEMETHOD(SetUINT32)(REFGUID g, UINT32 v) { return m_spAttributes->SetUINT32(g, v); }
        IFACEMETHOD(SetUINT64)(REFGUID g, UINT64 v) { return m_spAttributes->SetUINT64(g, v); }
        IFACEMETHOD(SetDouble)(REFGUID g, double v) { return m_spAttributes->SetDouble(g, v); }
        IFACEMETHOD(SetGUID)(REFGUID g, REFGUID v) { return m_spAttributes->SetGUID(g, v); }
        IFACEMETHOD(SetString)(REFGUID g, LPCWSTR v) { return m_spAttributes->SetString(g, v); }
        IFACEMETHOD(SetBlob)(REFGUID g, const UINT8* b, UINT32 s) { return m_spAttributes->SetBlob(g, b, s); }
        IFACEMETHOD(SetUnknown)(REFGUID g, IUnknown* u) { return m_spAttributes->SetUnknown(g, u); }
        IFACEMETHOD(LockStore)() { return m_spAttributes->LockStore(); }
        IFACEMETHOD(UnlockStore)() { return m_spAttributes->UnlockStore(); }
        IFACEMETHOD(GetCount)(UINT32* c) { return m_spAttributes->GetCount(c); }
        IFACEMETHOD(GetItemByIndex)(UINT32 i, GUID* g, PROPVARIANT* v) { return m_spAttributes->GetItemByIndex(i, g, v); }
        IFACEMETHOD(CopyAllItems)(IMFAttributes* d) { return m_spAttributes->CopyAllItems(d); }
    private:
        ComPtr<IMFAttributes> m_spAttributes;
    };

    // MediaSource method implementations
    HRESULT MediaSource::RuntimeClassInitialize()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        // Find the first available producer to determine the video dimensions.
        // Default to 1280x720 if none are found.
        UINT32 width = 1280, height = 720;
        try {
            auto producers = DirectPort::discover();
            if (!producers.empty()) {
                auto dpDevice = DirectPort::DeviceD3D11::create();
                auto consumer = dpDevice->connect_to_producer(producers[0].pid);
                if (consumer) {
                    auto texture = consumer->get_texture();
                    width = texture->get_width();
                    height = texture->get_height();
                }
            }
        } catch(...) { /* Fallback to default dimensions */ }
        
        RETURN_IF_FAILED(MFCreateEventQueue(&m_spEventQueue));
        RETURN_IF_FAILED(MakeAndInitialize<MediaStream>(&m_spStream, this, width, height));
        
        ComPtr<IMFStreamDescriptor> spStreamDesc;
        RETURN_IF_FAILED(m_spStream->GetStreamDescriptor(&spStreamDesc));
        
        IMFStreamDescriptor* pStreamDescs[] = { spStreamDesc.Get() };
        RETURN_IF_FAILED(MFCreatePresentationDescriptor(1, pStreamDescs, &m_spPresentationDescriptor));
        
        return m_spPresentationDescriptor->SelectStream(0);
    }
    HRESULT MediaSource::GetCharacteristics(DWORD* pdwCharacteristics) { *pdwCharacteristics = MFMEDIASOURCE_IS_LIVE; return S_OK; }
    HRESULT MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** ppd) { std::lock_guard<std::mutex> lock(m_mutex); if (!m_spPresentationDescriptor) return MF_E_SHUTDOWN; return m_spPresentationDescriptor->Clone(ppd); }
    HRESULT MediaSource::Start(IMFPresentationDescriptor* pd, const GUID*, const PROPVARIANT* sp) { std::lock_guard<std::mutex> lock(m_mutex); if (!pd || !sp) return E_INVALIDARG; HRESULT hr = m_spStream->Start(); if (SUCCEEDED(hr)) QueueEvent(MESourceStarted, GUID_NULL, hr, sp); return hr; }
    HRESULT MediaSource::Stop() { std::lock_guard<std::mutex> lock(m_mutex); HRESULT hr = m_spStream->Stop(); if (SUCCEEDED(hr)) QueueEvent(MESourceStopped, GUID_NULL, hr, nullptr); return hr; }
    HRESULT MediaSource::Pause() { return MF_E_INVALID_STATE_TRANSITION; }
    HRESULT MediaSource::Shutdown() { std::lock_guard<std::mutex> lock(m_mutex); if (m_spStream) m_spStream->Shutdown(); if (m_spEventQueue) m_spEventQueue->Shutdown(); return S_OK; }

    // MediaStream method implementations
    HRESULT MediaStream::RuntimeClassInitialize(MediaSource* pSource, UINT32 width, UINT32 height)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_spSource = pSource;
        m_width = width;
        m_height = height;
        RETURN_IF_FAILED(m_frameProvider.Initialize());
        RETURN_IF_FAILED(MFCreateEventQueue(&m_spEventQueue));

        ComPtr<IMFMediaType> spMediaType;
        RETURN_IF_FAILED(MFCreateMediaType(&spMediaType));
        RETURN_IF_FAILED(spMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
        RETURN_IF_FAILED(spMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32));
        RETURN_IF_FAILED(spMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
        RETURN_IF_FAILED(MFSetAttributeSize(spMediaType.Get(), MF_MT_FRAME_SIZE, m_width, m_height));
        RETURN_IF_FAILED(MFSetAttributeRatio(spMediaType.Get(), MF_MT_FRAME_RATE, VIDEO_FPS, 1));
        
        IMFMediaType* pMediaTypes[] = { spMediaType.Get() };
        return MFCreateStreamDescriptor(0, 1, pMediaTypes, &m_spStreamDescriptor);
    }
    HRESULT MediaStream::GetMediaSource(IMFMediaSource** ppMediaSource) { *ppMediaSource = m_spSource.Get(); (*ppMediaSource)->AddRef(); return S_OK; }
    HRESULT MediaStream::GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor) { return m_spStreamDescriptor.CopyTo(ppStreamDescriptor); }
    HRESULT MediaStream::Start() { std::lock_guard<std::mutex> lock(m_mutex); m_state = MF_STREAM_STATE_RUNNING; QueueEvent(MEStreamStarted, GUID_NULL, S_OK, nullptr); return S_OK; }
    HRESULT MediaStream::Stop() { std::lock_guard<std::mutex> lock(m_mutex); m_state = MF_STREAM_STATE_STOPPED; QueueEvent(MEStreamStopped, GUID_NULL, S_OK, nullptr); return S_OK; }
    HRESULT MediaStream::Shutdown() { std::lock_guard<std::mutex> lock(m_mutex); Stop(); if (m_spEventQueue) m_spEventQueue->Shutdown(); return S_OK; }
    HRESULT MediaStream::RequestSample(IUnknown* pToken)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_state != MF_STREAM_STATE_RUNNING) return MF_E_MEDIA_SOURCE_WRONGSTATE;

        ComPtr<IMFSample> spSample;
        RETURN_IF_FAILED(MFCreateSample(&spSample));

        ComPtr<IMFMediaBuffer> spBuffer;
        RETURN_IF_FAILED(MFCreate2DMediaBuffer(m_width, m_height, MFVideoFormat_RGB32.Data1, FALSE, &spBuffer));
        RETURN_IF_FAILED(spSample->AddBuffer(spBuffer.Get()));

        ComPtr<IDXGISurface> dxgiSurface;
        RETURN_IF_FAILED(spBuffer.As(&dxgiSurface));

        ComPtr<ID3D11Texture2D> d3d11Texture;
        RETURN_IF_FAILED(dxgiSurface.As(&d3d11Texture));
        
        RETURN_IF_FAILED(m_frameProvider.GenerateFrame(d3d11Texture.Get()));
        
        RETURN_IF_FAILED(spSample->SetSampleTime(m_timeStamp));
        RETURN_IF_FAILED(spSample->SetSampleDuration(VIDEO_FRAME_DURATION));
        m_timeStamp += VIDEO_FRAME_DURATION;
        
        if (pToken) RETURN_IF_FAILED(spSample->SetUnknown(MFSampleExtension_Token, pToken));

        return QueueEvent(MEMediaSample, GUID_NULL, S_OK, spSample.Get());
    }
}

// ---
// DLL Exports
// ---
class CClassFactory : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory> {
public:
    IFACEMETHOD(CreateInstance)(IUnknown* outer, REFIID riid, void** ppv) {
        if (outer) return CLASS_E_NOAGGREGATION;
        return Make<DirectPortVCam::Activator>().CopyTo(riid, ppv);
    }
    IFACEMETHOD(LockServer)(BOOL) { return S_OK; }
};

HMODULE g_hModule = NULL;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        Module<ModuleType::InProc>::GetModule().Create();
    }
    else if (reason == DLL_PROCESS_DETACH) {
        Module<ModuleType::InProc>::GetModule().Terminate();
    }
    return TRUE;
}

STDAPI DllCanUnloadNow(void) {
    return Module<InProc>::GetModule().GetObjectCount() == 0 ? S_OK : S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (rclsid == CLSID_DirectPortVirtualCamera)
        return Make<CClassFactory>().CopyTo(riid, ppv);
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllRegisterServer(void) {
    wchar_t modulePath[MAX_PATH];
    if (GetModuleFileNameW(g_hModule, modulePath, ARRAYSIZE(modulePath)) == 0)
        return HRESULT_FROM_WIN32(GetLastError());
    
    wchar_t clsidString[40];
    StringFromGUID2(CLSID_DirectPortVirtualCamera, clsidString, ARRAYSIZE(clsidString));
    
    wchar_t keyPath[MAX_PATH];
    swprintf_s(keyPath, L"Software\\Classes\\CLSID\\%s", clsidString);
    
    HKEY hKey;
    LONG res = RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    if (res != ERROR_SUCCESS) return HRESULT_FROM_WIN32(res);
    
    RegSetValueExW(hKey, NULL, 0, REG_SZ, (const BYTE*)VCAM_FRIENDLY_NAME, (DWORD)((wcslen(VCAM_FRIENDLY_NAME) + 1) * sizeof(wchar_t)));
    
    HKEY hSub;
    res = RegCreateKeyExW(hKey, L"InprocServer32", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hSub, NULL);
    if (res != ERROR_SUCCESS) { RegCloseKey(hKey); return HRESULT_FROM_WIN32(res); }
    
    const wchar_t* threadingModel = L"Both";
    RegSetValueExW(hSub, NULL, 0, REG_SZ, (const BYTE*)modulePath, (DWORD)((wcslen(modulePath) + 1) * sizeof(wchar_t)));
    RegSetValueExW(hSub, L"ThreadingModel", 0, REG_SZ, (const BYTE*)threadingModel, (DWORD)((wcslen(threadingModel) + 1) * sizeof(wchar_t)));
    
    RegCloseKey(hSub);
    RegCloseKey(hKey);
    return S_OK;
}

STDAPI DllUnregisterServer(void) {
    wchar_t clsidString[40];
    StringFromGUID2(CLSID_DirectPortVirtualCamera, clsidString, ARRAYSIZE(clsidString));
    wchar_t keyPath[MAX_PATH];
    swprintf_s(keyPath, L"Software\\Classes\\CLSID\\%s", clsidString);
    LONG res = RegDeleteTreeW(HKEY_LOCAL_MACHINE, keyPath);
    return (res == ERROR_SUCCESS) ? S_OK : HRESULT_FROM_WIN32(res);
}