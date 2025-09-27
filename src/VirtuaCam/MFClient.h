#pragma once

#include "BrokerClient.h"

template <class IFACE = IMFAttributes>
struct CBaseAttributes : public IFACE
{
protected:
	wil::com_ptr_nothrow<IMFAttributes> _attributes;
	std::wstring _trace;

	CBaseAttributes() :
		_trace(L"Atts")
	{
		THROW_IF_FAILED(MFCreateAttributes(&_attributes, 0));
	}

	void SetBaseAttributesTraceName(std::wstring trace)
	{
		_trace = trace;
	}

public:
	STDMETHODIMP GetItem(REFGUID guidKey, PROPVARIANT* value)
	{
		RETURN_HR_IF(E_INVALIDARG, !value);
		assert(_attributes);
		return _attributes->GetItem(guidKey, value);
	}
	STDMETHODIMP GetItemType(REFGUID guidKey, MF_ATTRIBUTE_TYPE* pType)
	{
		RETURN_HR_IF(E_INVALIDARG, !pType);
		*pType = (MF_ATTRIBUTE_TYPE)0;
		assert(_attributes);
		return _attributes->GetItemType(guidKey, pType);
	}
	STDMETHODIMP CompareItem(REFGUID guidKey, REFPROPVARIANT Value, BOOL* pbResult)
	{
		RETURN_HR_IF(E_INVALIDARG, !pbResult);
		assert(_attributes);
		return _attributes->CompareItem(guidKey, Value, pbResult);
	}
	STDMETHODIMP Compare(IMFAttributes* pTheirs, MF_ATTRIBUTES_MATCH_TYPE MatchType, BOOL* pbResult)
	{
		RETURN_HR_IF(E_INVALIDARG, !pTheirs || !pbResult);
		assert(_attributes);
		return _attributes->Compare(pTheirs, MatchType, pbResult);
	}
	STDMETHODIMP GetUINT32(REFGUID guidKey, UINT32* punValue)
	{
		RETURN_HR_IF(E_INVALIDARG, !punValue);
		*punValue = 0;
		assert(_attributes);
		return _attributes->GetUINT32(guidKey, punValue);
	}
	STDMETHODIMP GetUINT64(REFGUID guidKey, UINT64* punValue)
	{
		RETURN_HR_IF(E_INVALIDARG, !punValue);
		*punValue = 0;
		assert(_attributes);
		return _attributes->GetUINT64(guidKey, punValue);
	}
	STDMETHODIMP GetDouble(REFGUID guidKey, double* pfValue)
	{
		RETURN_HR_IF(E_INVALIDARG, !pfValue);
		*pfValue = 0;
		assert(_attributes);
		return _attributes->GetDouble(guidKey, pfValue);
	}
	STDMETHODIMP GetGUID(REFGUID guidKey, GUID* pguidValue)
	{
		RETURN_HR_IF(E_INVALIDARG, !pguidValue);
		ZeroMemory(pguidValue, 16);
		assert(_attributes);
		return _attributes->GetGUID(guidKey, pguidValue);
	}
	STDMETHODIMP GetStringLength(REFGUID guidKey, UINT32* pcchLength)
	{
		RETURN_HR_IF(E_INVALIDARG, !pcchLength);
		*pcchLength = 0;
		assert(_attributes);
		return _attributes->GetStringLength(guidKey, pcchLength);
	}
	STDMETHODIMP GetString(REFGUID guidKey, LPWSTR pwszValue, UINT32 cchBufSize, UINT32* pcchLength)
	{
		assert(_attributes);
		return _attributes->GetString(guidKey, pwszValue, cchBufSize, pcchLength);
	}
	STDMETHODIMP GetAllocatedString(REFGUID guidKey, LPWSTR* ppwszValue, UINT32* pcchLength)
	{
		RETURN_HR_IF(E_INVALIDARG, !ppwszValue || !pcchLength);
		*ppwszValue = 0;
		*pcchLength = 0;
		assert(_attributes);
		return _attributes->GetAllocatedString(guidKey, ppwszValue, pcchLength);
	}
	STDMETHODIMP GetBlobSize(REFGUID guidKey, UINT32* pcbBlobSize)
	{
		RETURN_HR_IF(E_INVALIDARG, !pcbBlobSize);
		assert(_attributes);
		return _attributes->GetBlobSize(guidKey, pcbBlobSize);
	}
	STDMETHODIMP GetBlob(REFGUID guidKey, UINT8* pBuf, UINT32 cbBufSize, UINT32* pcbBlobSize)
	{
		assert(_attributes);
		return _attributes->GetBlob(guidKey, pBuf, cbBufSize, pcbBlobSize);
	}
	STDMETHODIMP GetAllocatedBlob(REFGUID guidKey, UINT8** ppBuf, UINT32* pcbSize)
	{
		RETURN_HR_IF(E_INVALIDARG, !ppBuf || !pcbSize);
		assert(_attributes);
		return _attributes->GetAllocatedBlob(guidKey, ppBuf, pcbSize);
	}
	STDMETHODIMP GetUnknown(REFGUID guidKey, REFIID riid, LPVOID* ppv)
	{
		RETURN_HR_IF(E_INVALIDARG, !ppv);
		assert(_attributes);
		return _attributes->GetUnknown(guidKey, riid, ppv);
	}
	STDMETHODIMP SetItem(REFGUID guidKey, REFPROPVARIANT value)
	{
		assert(_attributes);
		return _attributes->SetItem(guidKey, value);
	}
	STDMETHODIMP DeleteItem(REFGUID guidKey)
	{
		assert(_attributes);
		return _attributes->DeleteItem(guidKey);
	}
	STDMETHODIMP DeleteAllItems()
	{
		assert(_attributes);
		return _attributes->DeleteAllItems();
	}
	STDMETHODIMP SetUINT32(REFGUID guidKey, UINT32 value)
	{
		assert(_attributes);
		return _attributes->SetUINT32(guidKey, value);
	}
	STDMETHODIMP SetUINT64(REFGUID guidKey, UINT64 value)
	{
		assert(_attributes);
		return _attributes->SetUINT64(guidKey, value);
	}
	STDMETHODIMP SetDouble(REFGUID guidKey, double value)
	{
		assert(_attributes);
		return _attributes->SetDouble(guidKey, value);
	}
	STDMETHODIMP SetGUID(REFGUID guidKey, REFGUID value)
	{
		assert(_attributes);
		return _attributes->SetGUID(guidKey, value);
	}
	STDMETHODIMP SetString(REFGUID guidKey, LPCWSTR value)
	{
		assert(_attributes);
		return _attributes->SetString(guidKey, value);
	}
	STDMETHODIMP SetBlob(REFGUID guidKey, const UINT8* pBuf, UINT32 cbBufSize)
	{
		assert(_attributes);
		return _attributes->SetBlob(guidKey, pBuf, cbBufSize);
	}
	STDMETHODIMP SetUnknown(REFGUID guidKey, IUnknown* value)
	{
		assert(_attributes);
		return _attributes->SetUnknown(guidKey, value);
	}
	STDMETHODIMP LockStore()
	{
		assert(_attributes);
		return _attributes->LockStore();
	}
	STDMETHODIMP UnlockStore()
	{
		assert(_attributes);
		return _attributes->UnlockStore();
	}
	STDMETHODIMP GetCount(UINT32* pcItems)
	{
		RETURN_HR_IF(E_INVALIDARG, !pcItems);
		assert(_attributes);
		return _attributes->GetCount(pcItems);
	}
	STDMETHODIMP GetItemByIndex(UINT32 unIndex, GUID* pguidKey, PROPVARIANT* pValue)
	{
		assert(_attributes);
		return _attributes->GetItemByIndex(unIndex, pguidKey, pValue);
	}
	STDMETHODIMP CopyAllItems(IMFAttributes* pDest)
	{
		RETURN_HR_IF(E_INVALIDARG, !pDest);
		assert(_attributes);
		return _attributes->CopyAllItems(pDest);
	}
};

struct MFSource;
struct MFStream;

struct MFActivate : winrt::implements<MFActivate, CBaseAttributes<IMFActivate>>
{
public:
	STDMETHOD(ActivateObject(REFIID riid, void** ppv));
	STDMETHOD(ShutdownObject)();
	STDMETHOD(DetachObject)();
	MFActivate() { SetBaseAttributesTraceName(L"ActivatorAtts"); }
	HRESULT Initialize();
private:
#if _DEBUG
	int32_t query_interface_tearoff(winrt::guid const& id, void** object) const noexcept;
#endif
	winrt::com_ptr<MFSource> _source;
};

struct MFSource : winrt::implements<MFSource, CBaseAttributes<IMFAttributes>, IMFMediaSource2, IMFGetService, IKsControl, IMFSampleAllocatorControl>
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
	STDMETHOD_(NTSTATUS, KsProperty)(PKSPROPERTY Property, ULONG PropertyLength, LPVOID PropertyData, ULONG DataLength, ULONG* BytesReturned);
	STDMETHOD_(NTSTATUS, KsMethod)(PKSMETHOD Method, ULONG MethodLength, LPVOID MethodData, ULONG DataLength, ULONG* BytesReturned);
	STDMETHOD_(NTSTATUS, KsEvent)(PKSEVENT Event, ULONG EventLength, LPVOID EventData, ULONG DataLength, ULONG* BytesReturned);

public:
	MFSource();
	HRESULT Initialize(IMFAttributes* attributes);
private:
#if _DEBUG
	int32_t query_interface_tearoff(winrt::guid const& id, void** object) const noexcept;
#endif
	int GetStreamIndexById(DWORD id);
private:
	const int _numStreams = 1;
	winrt::slim_mutex _lock;
	winrt::com_array<wil::com_ptr_nothrow<MFStream>> _streams;
	wil::com_ptr_nothrow<IMFMediaEventQueue> _queue;
	wil::com_ptr_nothrow<IMFPresentationDescriptor> _descriptor;
};

struct MFStream : winrt::implements<MFStream, CBaseAttributes<IMFAttributes>, IMFMediaStream2, IKsControl>
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

public:
	MFStream() : _index(0), _state(MF_STREAM_STATE_STOPPED), _format(GUID_NULL), _initialWidth(0), _initialHeight(0), _currentWidth(0), _currentHeight(0) { SetBaseAttributesTraceName(L"MediaStreamAtts"); }
	HRESULT Initialize(IMFMediaSource* source, int index);
	HRESULT SetAllocator(IUnknown* allocator);
	MFSampleAllocatorUsage GetAllocatorUsage();
	HRESULT SetD3DManager(IUnknown* manager);
	HRESULT Start(IMFMediaType* type);
	HRESULT Stop();
	void Shutdown();
private:
#if _DEBUG
	int32_t query_interface_tearoff(winrt::guid const& id, void** object) const noexcept override { RETURN_HR_MSG(E_NOINTERFACE, "MediaStream QueryInterface failed on IID %s", GUID_ToStringW(id).c_str()); }
#endif
	winrt::slim_mutex  _lock;
	MF_STREAM_STATE _state;
	BrokerClient _brokerClient;
	GUID _format;
	wil::com_ptr_nothrow<IMFStreamDescriptor> _descriptor;
	wil::com_ptr_nothrow<IMFMediaEventQueue> _queue;
	wil::com_ptr_nothrow<IMFMediaSource> _source;
	wil::com_ptr_nothrow<IMFVideoSampleAllocatorEx> _allocator;
	int _index;
    UINT _initialWidth;
    UINT _initialHeight;
    UINT _currentWidth;
    UINT _currentHeight;
};