#pragma once

struct MFStream;

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
	MFSource() :
		_streams(_numStreams)
	{
		SetBaseAttributesTraceName(L"MediaSourceAtts");
		for (auto i = 0; i < _numStreams; i++)
		{
			auto stream = winrt::make_self<MFStream>();
			stream->Initialize(this, i);
			_streams[i].attach(stream.detach());
		}
	}

	HRESULT Initialize(IMFAttributes* attributes);

private:
#if _DEBUG
	int32_t query_interface_tearoff(winrt::guid const& id, void** object) const noexcept override
	{
		if (id == winrt::guid_of<IMFDeviceSourceInternal>() ||
			id == winrt::guid_of<IMFDeviceSourceInternal2>() ||
			id == winrt::guid_of<IMFDeviceTransformManager>() ||
			id == winrt::guid_of<IMFCollection>() ||
			id == winrt::guid_of<IMFDeviceController2>() ||
			id == winrt::guid_of<IMFDeviceSourceStatus>())
			return E_NOINTERFACE;

		if (id == winrt::guid_of<IMFRealTimeClientEx>())
			return E_NOINTERFACE;

		RETURN_HR_MSG(E_NOINTERFACE, "MediaSource QueryInterface failed on IID %s", GUID_ToStringW(id).c_str());
	}
#endif

	int GetStreamIndexById(DWORD id);

private:
	const int _numStreams = 1;
	winrt::slim_mutex _lock;
	winrt::com_array<wil::com_ptr_nothrow<MFStream>> _streams;
	wil::com_ptr_nothrow<IMFMediaEventQueue> _queue;
	wil::com_ptr_nothrow<IMFPresentationDescriptor> _descriptor;
};