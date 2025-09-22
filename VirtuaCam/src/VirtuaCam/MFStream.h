#pragma once
#include "BrokerClient.h"
#include "Utilities.h"

struct MFSource; 

struct MFStream : winrt::implements<MFStream, IMFMediaStream2, IKsControl, IMFAttributes>
{
public:	
    STDMETHOD(BeginGetEvent)(IMFAsyncCallback* pCallback, IUnknown* punkState);
    STDMETHOD(EndGetEvent)(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent);
    STDMETHOD(GetEvent)(DWORD dwFlags, IMFMediaEvent** ppEvent);
    STDMETHOD(QueueEvent)(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue);
    STDMETHOD(GetMediaSource)(IMFMediaSource** ppMediaSource);
    STDMETHOD(GetStreamDescriptor)(IMFStreamDescriptor** ppStreamDescriptor);
    STDMETHOD(RequestSample)(IUnknown* pToken);
    STDMETHOD(SetStreamState)(MF_STREAM_STATE value);
    STDMETHOD(GetStreamState)(MF_STREAM_STATE* value);
    STDMETHOD_(NTSTATUS, KsProperty)(PKSPROPERTY Property, ULONG PropertyLength, LPVOID PropertyData, ULONG DataLength, ULONG* BytesReturned);
    STDMETHOD_(NTSTATUS, KsMethod)(PKSMETHOD Method, ULONG MethodLength, LPVOID MethodData, ULONG DataLength, ULONG* BytesReturned);
    STDMETHOD_(NTSTATUS, KsEvent)(PKSEVENT Event, ULONG EventLength, LPVOID EventData, ULONG DataLength, ULONG* BytesReturned);

    STDMETHODIMP GetItem(REFGUID guidKey, PROPVARIANT* pValue) override;
    STDMETHODIMP GetItemType(REFGUID guidKey, MF_ATTRIBUTE_TYPE* pType) override;
    STDMETHODIMP CompareItem(REFGUID guidKey, REFPROPVARIANT Value, BOOL* pbResult) override;
    STDMETHODIMP Compare(IMFAttributes* pTheirs, MF_ATTRIBUTES_MATCH_TYPE MatchType, BOOL* pbResult) override;
    STDMETHODIMP GetUINT32(REFGUID guidKey, UINT32* punValue) override;
    STDMETHODIMP GetUINT64(REFGUID guidKey, UINT64* punValue) override;
    STDMETHODIMP GetDouble(REFGUID guidKey, double* pfValue) override;
    STDMETHODIMP GetGUID(REFGUID guidKey, GUID* pguidValue) override;
    STDMETHODIMP GetStringLength(REFGUID guidKey, UINT32* pcchLength) override;
    STDMETHODIMP GetString(REFGUID guidKey, LPWSTR pwszValue, UINT32 cchBufSize, UINT32* pcchLength) override;
    STDMETHODIMP GetAllocatedString(REFGUID guidKey, LPWSTR* ppwszValue, UINT32* pcchLength) override;
    STDMETHODIMP GetBlobSize(REFGUID guidKey, UINT32* pcbBlobSize) override;
    STDMETHODIMP GetBlob(REFGUID guidKey, UINT8* pBuf, UINT32 cbBufSize, UINT32* pcbBlobSize) override;
    STDMETHODIMP GetAllocatedBlob(REFGUID guidKey, UINT8** ppBuf, UINT32* pcbSize) override;
    STDMETHODIMP GetUnknown(REFGUID guidKey, REFIID riid, LPVOID* ppv) override;
    STDMETHODIMP SetItem(REFGUID guidKey, REFPROPVARIANT Value) override;
    STDMETHODIMP DeleteItem(REFGUID guidKey) override;
    STDMETHODIMP DeleteAllItems() override;
    STDMETHODIMP SetUINT32(REFGUID guidKey, UINT32 unValue) override;
    STDMETHODIMP SetUINT64(REFGUID guidKey, UINT64 unValue) override;
    STDMETHODIMP SetDouble(REFGUID guidKey, double fValue) override;
    STDMETHODIMP SetGUID(REFGUID guidKey, REFGUID guidValue) override;
    STDMETHODIMP SetString(REFGUID guidKey, LPCWSTR pszValue) override;
    STDMETHODIMP SetBlob(REFGUID guidKey, const UINT8* pBuf, UINT32 cbBufSize) override;
    STDMETHODIMP SetUnknown(REFGUID guidKey, IUnknown* pUnknown) override;
    STDMETHODIMP LockStore() override;
    STDMETHODIMP UnlockStore() override;
    STDMETHODIMP GetCount(UINT32* pcItems) override;
    STDMETHODIMP GetItemByIndex(UINT32 unIndex, GUID* pguidKey, PROPVARIANT* pValue) override;
    STDMETHODIMP CopyAllItems(IMFAttributes* pDest) override;

    MFStream();
    HRESULT Initialize(MFSource* source, int index);
    HRESULT SetAllocator(IUnknown* allocator);
    MFSampleAllocatorUsage GetAllocatorUsage();
    HRESULT SetD3DManager(IUnknown* manager);
    HRESULT Start(IMFMediaType* type);
    HRESULT Stop();
    void Shutdown();

private:
    HRESULT _QueueEvent(MediaEventType eventType, const PROPVARIANT* value);
    HRESULT _QueueSampleEvent(IMFSample* sample);

    winrt::slim_mutex m_lock;
    wil::com_ptr_nothrow<IMFAttributes> m_attributes;
    MF_STREAM_STATE m_currentState;
    BrokerClient m_frameService;
    GUID m_currentFormat;
    wil::com_ptr_nothrow<IMFStreamDescriptor> m_streamDescriptor;
    wil::com_ptr_nothrow<IMFMediaEventQueue> m_eventQueue;
    MFSource* m_parentSource;
    wil::com_ptr_nothrow<IMFVideoSampleAllocatorEx> m_sampleAllocator;
    int m_streamIndex;
    UINT m_initialWidth;
    UINT m_initialHeight;
    UINT m_currentWidth;
    UINT m_currentHeight;
};