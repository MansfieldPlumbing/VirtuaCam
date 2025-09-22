#pragma once
#include "Utilities.h"
#include "MFStream.h"

struct MFSource : winrt::implements<MFSource, IMFMediaSource2, IMFGetService, IKsControl, IMFSampleAllocatorControl, IMFAttributes>
{
public:
    STDMETHOD(BeginGetEvent)(IMFAsyncCallback* pCallback, IUnknown* punkState);
    STDMETHOD(EndGetEvent)(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent);
    STDMETHOD(GetEvent)(DWORD dwFlags, IMFMediaEvent** ppEvent);
    STDMETHOD(QueueEvent)(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue);
    STDMETHOD(CreatePresentationDescriptor)(IMFPresentationDescriptor** ppPresentationDescriptor);
    STDMETHOD(GetCharacteristics)(DWORD* pdwCharacteristics);
    STDMETHOD(Pause)();
    STDMETHOD(Shutdown)();
    STDMETHOD(Start)(IMFPresentationDescriptor* pPresentationDescriptor, const GUID* pguidTimeFormat, const PROPVARIANT* pvarStartPosition);
    STDMETHOD(Stop)();
    STDMETHOD(GetSourceAttributes)(IMFAttributes** ppAttributes);
    STDMETHOD(GetStreamAttributes)(DWORD dwStreamIdentifier, IMFAttributes** ppAttributes);
    STDMETHOD(SetD3DManager)(IUnknown* pManager);
    STDMETHOD(SetMediaType)(DWORD dwStreamID, IMFMediaType* pMediaType);
    STDMETHOD(GetService)(REFGUID guidService, REFIID riid, LPVOID* ppvObject);
    STDMETHOD(SetDefaultAllocator)(DWORD dwOutputStreamID, IUnknown* pAllocator);
    STDMETHOD(GetAllocatorUsage)(DWORD dwOutputStreamID, DWORD* pdwInputStreamID, MFSampleAllocatorUsage* peUsage);
    
    STDMETHOD_(NTSTATUS, KsProperty)(PKSPROPERTY Property, ULONG PropertyLength, LPVOID PropertyData, ULONG DataLength, ULONG* BytesReturned) override;
    STDMETHOD_(NTSTATUS, KsMethod)(PKSMETHOD Method, ULONG MethodLength, LPVOID MethodData, ULONG DataLength, ULONG* BytesReturned) override;
    STDMETHOD_(NTSTATUS, KsEvent)(PKSEVENT Event, ULONG EventLength, LPVOID EventData, ULONG DataLength, ULONG* BytesReturned) override;

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

    MFSource();
    HRESULT Initialize(IMFAttributes* attributes);

private:
    int GetStreamIndexById(DWORD id);
    HRESULT _QueueEvent(MediaEventType eventType, const PROPVARIANT* eventValue);

    void StartIpcServer();
    void StopIpcServer();
    static DWORD WINAPI IpcThread(LPVOID lpParam);
    void HandleIpcCommand(DWORD command);

    const int m_streamCount = 1;
    winrt::slim_mutex m_lock;
    wil::com_ptr_nothrow<IMFAttributes> m_attributes;
    winrt::com_array<winrt::com_ptr<MFStream>> m_videoStreams;
    wil::com_ptr_nothrow<IMFMediaEventQueue> m_eventQueue;
    wil::com_ptr_nothrow<IMFPresentationDescriptor> m_presentationDescriptor;

    HANDLE m_hIpcThread = nullptr;
    HANDLE m_hIpcPipe = INVALID_HANDLE_VALUE;
    wil::unique_event m_ipcShutdownEvent;
    std::wstring m_pipeName;

    HANDLE m_hControlsMapping = nullptr;
    VirtuaCamControls* m_pControlsView = nullptr;
};