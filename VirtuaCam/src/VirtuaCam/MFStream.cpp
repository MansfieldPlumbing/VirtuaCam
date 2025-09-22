#include "pch.h"
#include "Guids.h"
#include "MFSource.h"
#include "MFStream.h"
#include "App.h"
#include "Formats.h"

MFStream::MFStream() : m_currentState(MF_STREAM_STATE_STOPPED), m_streamIndex(0), m_initialWidth(1920), m_initialHeight(1080), m_currentWidth(0), m_currentHeight(0), m_parentSource(nullptr)
{
    THROW_IF_FAILED(MFCreateAttributes(&m_attributes, 0));
    THROW_IF_FAILED(MFCreateEventQueue(&m_eventQueue));
}

HRESULT MFStream::Initialize(MFSource* source, int index)
{
    m_parentSource = source;
    m_streamIndex = index;
    
    auto mediaTypes = VirtuaCam::Utils::make_unique_cotaskmem_array<wil::com_ptr_nothrow<IMFMediaType>>(g_supportedResolutions.size() * g_supportedFrameRates.size() * 2);
    uint32_t typeIndex = 0;

    for (const auto& res : g_supportedResolutions)
    {
        for (const auto& fr : g_supportedFrameRates)
        {
            wil::com_ptr_nothrow<IMFMediaType> nv12Type;
            RETURN_IF_FAILED(MFCreateMediaType(&nv12Type));
            RETURN_IF_FAILED(nv12Type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
            RETURN_IF_FAILED(nv12Type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
            RETURN_IF_FAILED(MFSetAttributeSize(nv12Type.get(), MF_MT_FRAME_SIZE, res.width, res.height));
            RETURN_IF_FAILED(MFSetAttributeRatio(nv12Type.get(), MF_MT_FRAME_RATE, fr.numerator, fr.denominator));
            mediaTypes[typeIndex++] = nv12Type.detach();

            wil::com_ptr_nothrow<IMFMediaType> rgb32Type;
            RETURN_IF_FAILED(MFCreateMediaType(&rgb32Type));
            RETURN_IF_FAILED(rgb32Type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
            RETURN_IF_FAILED(rgb32Type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32));
            RETURN_IF_FAILED(MFSetAttributeSize(rgb32Type.get(), MF_MT_FRAME_SIZE, res.width, res.height));
            RETURN_IF_FAILED(MFSetAttributeRatio(rgb32Type.get(), MF_MT_FRAME_RATE, fr.numerator, fr.denominator));
            mediaTypes[typeIndex++] = rgb32Type.detach();
        }
    }

    RETURN_IF_FAILED(MFCreateStreamDescriptor(m_streamIndex, typeIndex, mediaTypes.get(), &m_streamDescriptor));
    RETURN_IF_FAILED(SetUINT32(MF_DEVICESTREAM_STREAM_ID, m_streamIndex));
    RETURN_IF_FAILED(SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
    RETURN_IF_FAILED(SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
    
    return S_OK;
}

void MFStream::Shutdown()
{
    winrt::slim_lock_guard lock(m_lock);
    if (m_eventQueue)
    {
        m_eventQueue->Shutdown();
        m_eventQueue.reset();
    }
    m_parentSource = nullptr;
    m_streamDescriptor.reset();
    m_attributes.reset();
    m_sampleAllocator.reset();
}

HRESULT MFStream::SetAllocator(IUnknown* pAllocator)
{
    winrt::slim_lock_guard lock(m_lock);
    m_sampleAllocator.reset();
    if (pAllocator)
    {
        return pAllocator->QueryInterface(IID_PPV_ARGS(&m_sampleAllocator));
    }
    return S_OK;
}

MFSampleAllocatorUsage MFStream::GetAllocatorUsage()
{
    return MFSampleAllocatorUsage_UsesCustomAllocator;
}

HRESULT MFStream::SetD3DManager(IUnknown* manager)
{
    winrt::slim_lock_guard lock(m_lock);
    if (m_frameService.HasD3DManager()) return S_OK;
    return m_frameService.SetD3DManager(manager, m_initialWidth, m_initialHeight);
}

HRESULT MFStream::Start(IMFMediaType* type)
{
    winrt::slim_lock_guard lock(m_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_eventQueue);
    
    UINT32 width, height;
    RETURN_IF_FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height));
    RETURN_IF_FAILED(type->GetGUID(MF_MT_SUBTYPE, &m_currentFormat));

    if (width != m_currentWidth || height != m_currentHeight)
    {
        m_currentWidth = width;
        m_currentHeight = height;
        if(m_frameService.HasD3DManager())
        {
            RETURN_IF_FAILED(m_frameService.ReconfigureFormat(m_currentWidth, m_currentHeight));
        }
    }

    wil::unique_prop_variant time;
    RETURN_IF_FAILED(InitPropVariantFromInt64(MFGetSystemTime(), &time));
    RETURN_IF_FAILED(SetStreamState(MF_STREAM_STATE_RUNNING));
    RETURN_IF_FAILED(_QueueEvent(MEStreamStarted, &time));
    return S_OK;
}

HRESULT MFStream::Stop()
{
    winrt::slim_lock_guard lock(m_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_eventQueue);

    wil::unique_prop_variant time;
    RETURN_IF_FAILED(InitPropVariantFromInt64(MFGetSystemTime(), &time));
    RETURN_IF_FAILED(SetStreamState(MF_STREAM_STATE_STOPPED));
    RETURN_IF_FAILED(_QueueEvent(MEStreamStopped, &time));
    return S_OK;
}

STDMETHODIMP MFStream::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState)
{
    winrt::slim_lock_guard lock(m_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_eventQueue);
    return m_eventQueue->BeginGetEvent(pCallback, punkState);
}

STDMETHODIMP MFStream::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
    RETURN_HR_IF_NULL(E_POINTER, ppEvent);
    *ppEvent = nullptr;
    winrt::slim_lock_guard lock(m_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_eventQueue);
    return m_eventQueue->EndGetEvent(pResult, ppEvent);
}

STDMETHODIMP MFStream::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
    RETURN_HR_IF_NULL(E_POINTER, ppEvent);
    *ppEvent = nullptr;
    winrt::slim_lock_guard lock(m_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_eventQueue);
    return m_eventQueue->GetEvent(dwFlags, ppEvent);
}

STDMETHODIMP MFStream::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue)
{
    winrt::slim_lock_guard lock(m_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_eventQueue);
    return m_eventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
}

STDMETHODIMP MFStream::GetMediaSource(IMFMediaSource** ppMediaSource)
{
    RETURN_HR_IF_NULL(E_POINTER, ppMediaSource);
    *ppMediaSource = nullptr;
    winrt::slim_lock_guard lock(m_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_parentSource);
    return m_parentSource->QueryInterface(IID_PPV_ARGS(ppMediaSource));
}

STDMETHODIMP MFStream::GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor)
{
    RETURN_HR_IF_NULL(E_POINTER, ppStreamDescriptor);
    *ppStreamDescriptor = nullptr;
    winrt::slim_lock_guard lock(m_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_streamDescriptor);
    return m_streamDescriptor.copy_to(ppStreamDescriptor);
}

STDMETHODIMP MFStream::RequestSample(IUnknown* pToken)
{
    winrt::slim_lock_guard lock(m_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_eventQueue);
    
    if (m_currentState != MF_STREAM_STATE_RUNNING)
    {
        return MF_E_INVALID_STATE_TRANSITION;
    }
    
    wil::com_ptr_nothrow<IMFSample> sample;
    wil::com_ptr_nothrow<IMFSample> outSample;
    RETURN_IF_FAILED(m_sampleAllocator->AllocateSample(&sample));
    if (pToken)
    {
        RETURN_IF_FAILED(sample->SetUnknown(MFSampleExtension_RequestToken, pToken));
    }
    
    RETURN_IF_FAILED(m_frameService.Generate(sample.get(), m_currentFormat, &outSample));
    RETURN_IF_FAILED(_QueueSampleEvent(outSample.get()));
    
    return S_OK;
}

STDMETHODIMP MFStream::SetStreamState(MF_STREAM_STATE value)
{
    winrt::slim_lock_guard lock(m_lock);
    m_currentState = value;
    return S_OK;
}

STDMETHODIMP MFStream::GetStreamState(MF_STREAM_STATE* value)
{
    RETURN_HR_IF_NULL(E_POINTER, value);
    winrt::slim_lock_guard lock(m_lock);
    *value = m_currentState;
    return S_OK;
}

STDMETHODIMP_(NTSTATUS) MFStream::KsProperty(PKSPROPERTY Property, ULONG PropertyLength, LPVOID PropertyData, ULONG DataLength, ULONG* bytesReturned)
{
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_parentSource);
    return m_parentSource->KsProperty(Property, PropertyLength, PropertyData, DataLength, bytesReturned);
}

STDMETHODIMP_(NTSTATUS) MFStream::KsMethod(PKSMETHOD Method, ULONG MethodLength, LPVOID MethodData, ULONG DataLength, ULONG* bytesReturned)
{
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_parentSource);
    return m_parentSource->KsMethod(Method, MethodLength, MethodData, DataLength, bytesReturned);
}

STDMETHODIMP_(NTSTATUS) MFStream::KsEvent(PKSEVENT Event, ULONG EventLength, LPVOID EventData, ULONG DataLength, ULONG* bytesReturned)
{
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_parentSource);
    return m_parentSource->KsEvent(Event, EventLength, EventData, DataLength, bytesReturned);
}

HRESULT MFStream::_QueueEvent(MediaEventType eventType, const PROPVARIANT* value)
{
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_eventQueue);
    return m_eventQueue->QueueEventParamVar(eventType, GUID_NULL, S_OK, value);
}

HRESULT MFStream::_QueueSampleEvent(IMFSample* sample)
{
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_eventQueue);
    return m_eventQueue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample);
}

STDMETHODIMP MFStream::Compare(IMFAttributes* pTheirs, MF_ATTRIBUTES_MATCH_TYPE MatchType, BOOL* pbResult) { return m_attributes->Compare(pTheirs, MatchType, pbResult); }
STDMETHODIMP MFStream::CompareItem(REFGUID guidKey, REFPROPVARIANT Value, BOOL* pbResult) { return m_attributes->CompareItem(guidKey, Value, pbResult); }
STDMETHODIMP MFStream::CopyAllItems(IMFAttributes* pDest) { return m_attributes->CopyAllItems(pDest); }
STDMETHODIMP MFStream::DeleteAllItems() { return m_attributes->DeleteAllItems(); }
STDMETHODIMP MFStream::DeleteItem(REFGUID guidKey) { return m_attributes->DeleteItem(guidKey); }
STDMETHODIMP MFStream::GetAllocatedBlob(REFGUID guidKey, UINT8** ppBuf, UINT32* pcbSize) { return m_attributes->GetAllocatedBlob(guidKey, ppBuf, pcbSize); }
STDMETHODIMP MFStream::GetAllocatedString(REFGUID guidKey, LPWSTR* ppwszValue, UINT32* pcchLength) { return m_attributes->GetAllocatedString(guidKey, ppwszValue, pcchLength); }
STDMETHODIMP MFStream::GetBlob(REFGUID guidKey, UINT8* pBuf, UINT32 cbBufSize, UINT32* pcbBlobSize) { return m_attributes->GetBlob(guidKey, pBuf, cbBufSize, pcbBlobSize); }
STDMETHODIMP MFStream::GetBlobSize(REFGUID guidKey, UINT32* pcbBlobSize) { return m_attributes->GetBlobSize(guidKey, pcbBlobSize); }
STDMETHODIMP MFStream::GetCount(UINT32* pcItems) { return m_attributes->GetCount(pcItems); }
STDMETHODIMP MFStream::GetDouble(REFGUID guidKey, double* pfValue) { return m_attributes->GetDouble(guidKey, pfValue); }
STDMETHODIMP MFStream::GetGUID(REFGUID guidKey, GUID* pguidValue) { return m_attributes->GetGUID(guidKey, pguidValue); }
STDMETHODIMP MFStream::GetItem(REFGUID guidKey, PROPVARIANT* pValue) { return m_attributes->GetItem(guidKey, pValue); }
STDMETHODIMP MFStream::GetItemByIndex(UINT32 unIndex, GUID* pguidKey, PROPVARIANT* pValue) { return m_attributes->GetItemByIndex(unIndex, pguidKey, pValue); }
STDMETHODIMP MFStream::GetItemType(REFGUID guidKey, MF_ATTRIBUTE_TYPE* pType) { return m_attributes->GetItemType(guidKey, pType); }
STDMETHODIMP MFStream::GetString(REFGUID guidKey, LPWSTR pwszValue, UINT32 cchBufSize, UINT32* pcchLength) { return m_attributes->GetString(guidKey, pwszValue, cchBufSize, pcchLength); }
STDMETHODIMP MFStream::GetStringLength(REFGUID guidKey, UINT32* pcchLength) { return m_attributes->GetStringLength(guidKey, pcchLength); }
STDMETHODIMP MFStream::GetUINT32(REFGUID guidKey, UINT32* punValue) { return m_attributes->GetUINT32(guidKey, punValue); }
STDMETHODIMP MFStream::GetUINT64(REFGUID guidKey, UINT64* punValue) { return m_attributes->GetUINT64(guidKey, punValue); }
STDMETHODIMP MFStream::GetUnknown(REFGUID guidKey, REFIID riid, LPVOID* ppv) { return m_attributes->GetUnknown(guidKey, riid, ppv); }
STDMETHODIMP MFStream::LockStore() { return m_attributes->LockStore(); }
STDMETHODIMP MFStream::SetBlob(REFGUID guidKey, const UINT8* pBuf, UINT32 cbBufSize) { return m_attributes->SetBlob(guidKey, pBuf, cbBufSize); }
STDMETHODIMP MFStream::SetDouble(REFGUID guidKey, double fValue) { return m_attributes->SetDouble(guidKey, fValue); }
STDMETHODIMP MFStream::SetGUID(REFGUID guidKey, REFGUID guidValue) { return m_attributes->SetGUID(guidKey, guidValue); }
STDMETHODIMP MFStream::SetItem(REFGUID guidKey, REFPROPVARIANT Value) { return m_attributes->SetItem(guidKey, Value); }
STDMETHODIMP MFStream::SetString(REFGUID guidKey, LPCWSTR pszValue) { return m_attributes->SetString(guidKey, pszValue); }
STDMETHODIMP MFStream::SetUINT32(REFGUID guidKey, UINT32 unValue) { return m_attributes->SetUINT32(guidKey, unValue); }
STDMETHODIMP MFStream::SetUINT64(REFGUID guidKey, UINT64 unValue) { return m_attributes->SetUINT64(guidKey, unValue); }
STDMETHODIMP MFStream::SetUnknown(REFGUID guidKey, IUnknown* pUnknown) { return m_attributes->SetUnknown(guidKey, pUnknown); }
STDMETHODIMP MFStream::UnlockStore() { return m_attributes->UnlockStore(); }