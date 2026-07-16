#include "pch.h"
#include "Tools.h"
#include "MFClient.h"
#include "App.h"
#include "Formats.h"
#include <appmodel.h>

// Live COM object / server-lock count for DllCanUnloadNow().
std::atomic<long> g_moduleObjectCount{ 0 };

// ---------------------------------------------------------------------------
// IUnknown implementations
// ---------------------------------------------------------------------------
// Each class answers QueryInterface for the exact set of interfaces it
// implements, including every base interface in the MF inheritance chains
// (e.g. IMFMediaSource2 -> IMFMediaSourceEx -> IMFMediaSource ->
// IMFMediaEventGenerator).  The Media Foundation frame server also probes for
// several undocumented internal interfaces; they simply fall through to
// E_NOINTERFACE here.

STDMETHODIMP MFActivate::QueryInterface(REFIID riid, void** ppv)
{
	RETURN_HR_IF_NULL(E_POINTER, ppv);
	*ppv = nullptr;
	if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAttributes) || riid == __uuidof(IMFActivate))
		*ppv = static_cast<IMFActivate*>(this);
	else
		return E_NOINTERFACE;
	AddRef();
	return S_OK;
}

STDMETHODIMP_(ULONG) MFActivate::AddRef() { return ++_refCount; }
STDMETHODIMP_(ULONG) MFActivate::Release()
{
	auto count = --_refCount;
	if (count == 0)
		delete this;
	return count;
}

STDMETHODIMP MFSource::QueryInterface(REFIID riid, void** ppv)
{
	RETURN_HR_IF_NULL(E_POINTER, ppv);
	*ppv = nullptr;
	if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaSource2) || riid == __uuidof(IMFMediaSourceEx) || riid == __uuidof(IMFMediaSource) || riid == __uuidof(IMFMediaEventGenerator))
		*ppv = static_cast<IMFMediaSource2*>(this);
	else if (riid == __uuidof(IMFAttributes))
		*ppv = static_cast<IMFAttributes*>(this);
	else if (riid == __uuidof(IMFGetService))
		*ppv = static_cast<IMFGetService*>(this);
	else if (riid == __uuidof(IKsControl))
		*ppv = static_cast<IKsControl*>(this);
	else if (riid == __uuidof(IMFSampleAllocatorControl))
		*ppv = static_cast<IMFSampleAllocatorControl*>(this);
	else
		return E_NOINTERFACE;
	AddRef();
	return S_OK;
}

STDMETHODIMP_(ULONG) MFSource::AddRef() { return ++_refCount; }
STDMETHODIMP_(ULONG) MFSource::Release()
{
	auto count = --_refCount;
	if (count == 0)
		delete this;
	return count;
}

STDMETHODIMP MFStream::QueryInterface(REFIID riid, void** ppv)
{
	RETURN_HR_IF_NULL(E_POINTER, ppv);
	*ppv = nullptr;
	if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaStream2) || riid == __uuidof(IMFMediaStream) || riid == __uuidof(IMFMediaEventGenerator))
		*ppv = static_cast<IMFMediaStream2*>(this);
	else if (riid == __uuidof(IMFAttributes))
		*ppv = static_cast<IMFAttributes*>(this);
	else if (riid == __uuidof(IKsControl))
		*ppv = static_cast<IKsControl*>(this);
	else
		return E_NOINTERFACE;
	AddRef();
	return S_OK;
}

STDMETHODIMP_(ULONG) MFStream::AddRef() { return ++_refCount; }
STDMETHODIMP_(ULONG) MFStream::Release()
{
	auto count = --_refCount;
	if (count == 0)
		delete this;
	return count;
}

// ---------------------------------------------------------------------------
// MFActivate
// ---------------------------------------------------------------------------

HRESULT MFActivate::Initialize() try
{
	_source.attach(new MFSource());
	RETURN_IF_FAILED(SetUINT32(MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES, 1));
	RETURN_IF_FAILED(SetGUID(MFT_TRANSFORM_CLSID_Attribute, CLSID_VCam));
	RETURN_IF_FAILED(_source->Initialize(this));
	return S_OK;
}
CATCH_RETURN()

STDMETHODIMP MFActivate::ActivateObject(REFIID riid, void** ppv)
{
	RETURN_HR_IF_NULL(E_POINTER, ppv);
	*ppv = nullptr;
	RETURN_HR_IF(MF_E_SHUTDOWN, !_source);
	RETURN_IF_FAILED_MSG(_source->QueryInterface(riid, ppv), "Activator::ActivateObject failed on IID %ls", GUID_ToStringW(riid).c_str());
	return S_OK;
}

STDMETHODIMP MFActivate::ShutdownObject()
{
	return S_OK;
}

STDMETHODIMP MFActivate::DetachObject()
{
	_source = nullptr;
	return S_OK;
}

// ---------------------------------------------------------------------------
// MFSource
// ---------------------------------------------------------------------------

MFSource::MFSource()
{
    g_moduleObjectCount++;
    SetBaseAttributesTraceName(L"MediaSourceAtts");
    _streams.resize(_numStreams);
    for (auto i = 0; i < _numStreams; i++)
    {
        _streams[i].attach(new MFStream());
        _streams[i]->Initialize(this, i);
    }
}

HRESULT MFSource::Initialize(IMFAttributes* attributes)
{
	if (attributes)
	{
		RETURN_IF_FAILED(attributes->CopyAllItems(this));
	}

	wil::com_ptr_nothrow<IMFSensorProfileCollection> collection;
	RETURN_IF_FAILED(MFCreateSensorProfileCollection(&collection));
	DWORD streamId = 0;
	wil::com_ptr_nothrow<IMFSensorProfile> profile;

    RETURN_IF_FAILED(MFCreateSensorProfile(KSCAMERAPROFILE_VideoConferencing, 0, nullptr, &profile));
    RETURN_IF_FAILED(profile->AddProfileFilter(streamId, L"((RES==;FRT==30,1;SUT==))"));
    RETURN_IF_FAILED(collection->AddProfile(profile.get()));
    profile = nullptr;

	RETURN_IF_FAILED(MFCreateSensorProfile(KSCAMERAPROFILE_HighFrameRate, 0, nullptr, &profile));
	RETURN_IF_FAILED(profile->AddProfileFilter(streamId, L"((RES==;FRT>=60,1;SUT==))"));
	RETURN_IF_FAILED(collection->AddProfile(profile.get()));
    profile = nullptr;

	RETURN_IF_FAILED(MFCreateSensorProfile(KSCAMERAPROFILE_Legacy, 0, nullptr, &profile));
	RETURN_IF_FAILED(profile->AddProfileFilter(streamId, L"((RES==;FRT<60,1;SUT==))"));
	RETURN_IF_FAILED(collection->AddProfile(profile.get()));
    profile = nullptr;

	RETURN_IF_FAILED(SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, collection.get()));

	// If the hosting process has package identity, advertise its family name as
	// the camera's configuration app.  Plain Win32 hosts (the usual case) get
	// APPMODEL_ERROR_NO_PACKAGE here, which we ignore.
	UINT32 pfnLength = 0;
	if (GetCurrentPackageFamilyName(&pfnLength, nullptr) == ERROR_INSUFFICIENT_BUFFER && pfnLength > 0)
	{
		std::wstring packageFamilyName(pfnLength, L'\0');
		if (GetCurrentPackageFamilyName(&pfnLength, packageFamilyName.data()) == ERROR_SUCCESS)
		{
			packageFamilyName.resize(pfnLength > 0 ? pfnLength - 1 : 0);
			RETURN_IF_FAILED(SetString(MF_VIRTUALCAMERA_CONFIGURATION_APP_PACKAGE_FAMILY_NAME, packageFamilyName.c_str()));
		}
	}

	auto streams = wil::make_unique_cotaskmem_array<wil::com_ptr_nothrow<IMFStreamDescriptor>>(_streams.size());
	for (uint32_t i = 0; i < streams.size(); i++)
	{
		wil::com_ptr_nothrow<IMFStreamDescriptor> desc;
		RETURN_IF_FAILED(_streams[i]->GetStreamDescriptor(&desc));
		streams[i] = desc.detach();
	}
	RETURN_IF_FAILED(MFCreatePresentationDescriptor((DWORD)streams.size(), streams.get(), &_descriptor));
	RETURN_IF_FAILED(MFCreateEventQueue(&_queue));
	return S_OK;
}

int MFSource::GetStreamIndexById(DWORD id)
{
	for (uint32_t i = 0; i < _streams.size(); i++)
	{
		wil::com_ptr_nothrow<IMFStreamDescriptor> desc;
		if (FAILED(_streams[i]->GetStreamDescriptor(&desc)))
			return -1;

		DWORD sid = 0;
		if (FAILED(desc->GetStreamIdentifier(&sid)))
			return -1;

		if (sid == id)
			return i;
	}
	return -1;
}

STDMETHODIMP MFSource::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState)
{
	auto lock = _lock.lock_exclusive();
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);
	RETURN_IF_FAILED(_queue->BeginGetEvent(pCallback, punkState));
	return S_OK;
}

STDMETHODIMP MFSource::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
	RETURN_HR_IF_NULL(E_POINTER, ppEvent);
	*ppEvent = nullptr;
	auto lock = _lock.lock_exclusive();
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);
	RETURN_IF_FAILED(_queue->EndGetEvent(pResult, ppEvent));
	return S_OK;
}

STDMETHODIMP MFSource::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
	RETURN_HR_IF_NULL(E_POINTER, ppEvent);
	*ppEvent = nullptr;
	auto lock = _lock.lock_exclusive();
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);
	RETURN_IF_FAILED(_queue->GetEvent(dwFlags, ppEvent));
	return S_OK;
}

STDMETHODIMP MFSource::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue)
{
	auto lock = _lock.lock_exclusive();
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);
	RETURN_IF_FAILED(_queue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue));
	return S_OK;
}

STDMETHODIMP MFSource::CreatePresentationDescriptor(IMFPresentationDescriptor** ppPresentationDescriptor)
{
	RETURN_HR_IF_NULL(E_POINTER, ppPresentationDescriptor);
	*ppPresentationDescriptor = nullptr;
	auto lock = _lock.lock_exclusive();
	RETURN_HR_IF(MF_E_SHUTDOWN, !_descriptor);
	RETURN_IF_FAILED(_descriptor->Clone(ppPresentationDescriptor));
	return S_OK;
}

STDMETHODIMP MFSource::GetCharacteristics(DWORD* pdwCharacteristics)
{
	RETURN_HR_IF_NULL(E_POINTER, pdwCharacteristics);
	*pdwCharacteristics = MFMEDIASOURCE_IS_LIVE;
	return S_OK;
}

STDMETHODIMP MFSource::Pause()
{
	RETURN_HR(MF_E_INVALID_STATE_TRANSITION);
}

STDMETHODIMP MFSource::Shutdown()
{
	auto lock = _lock.lock_exclusive();
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);
	LOG_IF_FAILED_MSG(_queue->Shutdown(), "Queue shutdown failed");
	_queue.reset();
	for (uint32_t i = 0; i < _streams.size(); i++)
	{
		_streams[i]->Shutdown();
	}
	_descriptor.reset();
	_attributes.reset();
	return S_OK;
}

STDMETHODIMP MFSource::Start(IMFPresentationDescriptor* pPresentationDescriptor, const GUID* pguidTimeFormat, const PROPVARIANT* pvarStartPosition)
{
	RETURN_HR_IF_NULL(E_POINTER, pPresentationDescriptor);
	RETURN_HR_IF_NULL(E_POINTER, pvarStartPosition);
	RETURN_HR_IF_MSG(E_INVALIDARG, pguidTimeFormat && *pguidTimeFormat != GUID_NULL, "Unsupported guid time format");
	auto lock = _lock.lock_exclusive();
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue || !_descriptor);

	DWORD count;
	RETURN_IF_FAILED(pPresentationDescriptor->GetStreamDescriptorCount(&count));
	RETURN_HR_IF_MSG(E_INVALIDARG, count != (DWORD)_streams.size(), "Invalid number of descriptor streams");

	wil::unique_prop_variant time;
	RETURN_IF_FAILED(InitPropVariantFromInt64(MFGetSystemTime(), &time));

	for (DWORD i = 0; i < count; i++)
	{
		wil::com_ptr_nothrow<IMFStreamDescriptor> desc;
		BOOL selected = FALSE;
		RETURN_IF_FAILED(pPresentationDescriptor->GetStreamDescriptorByIndex(i, &selected, &desc));

		DWORD id = 0;
		RETURN_IF_FAILED(desc->GetStreamIdentifier(&id));

		auto index = GetStreamIndexById(id);
		RETURN_HR_IF(E_FAIL, index < 0);

		BOOL thisSelected = FALSE;
		wil::com_ptr_nothrow<IMFStreamDescriptor> thisDesc;
		RETURN_IF_FAILED(_descriptor->GetStreamDescriptorByIndex(index, &thisSelected, &thisDesc));

		MF_STREAM_STATE state;
		RETURN_IF_FAILED(_streams[i]->GetStreamState(&state));
		if (thisSelected && state == MF_STREAM_STATE_STOPPED )
		{
			thisSelected = FALSE;
		}
		else if (!thisSelected && state != MF_STREAM_STATE_STOPPED)
		{
			thisSelected = TRUE;
		}

		if (selected != thisSelected)
		{
			if (selected)
			{
				RETURN_IF_FAILED(_descriptor->SelectStream(index));

				wil::com_ptr_nothrow<IUnknown> unk;
				RETURN_IF_FAILED(_streams[index]->QueryInterface(IID_PPV_ARGS(&unk)));
				RETURN_IF_FAILED(_queue->QueueEventParamUnk(MENewStream, GUID_NULL, S_OK, unk.get()));

				wil::com_ptr_nothrow<IMFMediaTypeHandler> handler;
				wil::com_ptr_nothrow<IMFMediaType> type;
				RETURN_IF_FAILED(desc->GetMediaTypeHandler(&handler));
				RETURN_IF_FAILED(handler->GetCurrentMediaType(&type));

				RETURN_IF_FAILED(_streams[index]->Start(type.get()));
			}
			else
			{
				RETURN_IF_FAILED(_descriptor->DeselectStream(index));
				RETURN_IF_FAILED(_streams[index]->Stop());
			}
		}
	}
	RETURN_IF_FAILED(_queue->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &time));
	return S_OK;
}

STDMETHODIMP MFSource::Stop()
{
	auto lock = _lock.lock_exclusive();
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue || !_descriptor);
	wil::unique_prop_variant time;
	RETURN_IF_FAILED(InitPropVariantFromInt64(MFGetSystemTime(), &time));
	for (DWORD i = 0; i < _streams.size(); i++)
	{
		RETURN_IF_FAILED(_streams[i]->Stop());
		RETURN_IF_FAILED(_descriptor->DeselectStream(i));
	}
	RETURN_IF_FAILED(_queue->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, &time));
	return S_OK;
}

STDMETHODIMP MFSource::GetSourceAttributes(IMFAttributes** ppAttributes)
{
	RETURN_HR_IF_NULL(E_POINTER, ppAttributes);
	auto lock = _lock.lock_exclusive();
	RETURN_IF_FAILED(QueryInterface(IID_PPV_ARGS(ppAttributes)));
	return S_OK;
}

STDMETHODIMP MFSource::SetMediaType(DWORD dwStreamID, IMFMediaType* pMediaType)
{
	RETURN_HR_IF_NULL(E_POINTER, pMediaType);
	auto lock = _lock.lock_exclusive();
	return S_OK;
}

STDMETHODIMP MFSource::GetStreamAttributes(DWORD dwStreamIdentifier, IMFAttributes** ppAttributes)
{
	RETURN_HR_IF_NULL(E_POINTER, ppAttributes);
	*ppAttributes = nullptr;
	auto lock = _lock.lock_exclusive();
	RETURN_HR_IF_MSG(E_FAIL, dwStreamIdentifier >= _streams.size(), "dwStreamIdentifier %u is invalid", dwStreamIdentifier);
	RETURN_IF_FAILED(_streams[dwStreamIdentifier]->QueryInterface(IID_PPV_ARGS(ppAttributes)));
	return S_OK;
}

STDMETHODIMP MFSource::SetD3DManager(IUnknown* pManager)
{
	RETURN_HR_IF_NULL(E_POINTER, pManager);
	auto lock = _lock.lock_exclusive();
	for (DWORD i = 0; i < _streams.size(); i++)
	{
		RETURN_IF_FAILED(_streams[i]->SetD3DManager(pManager));
	}
	return S_OK;
}

STDMETHODIMP MFSource::GetService(REFGUID siid, REFIID iid, LPVOID* ppvObject)
{
	return MF_E_UNSUPPORTED_SERVICE;
}

STDMETHODIMP MFSource::SetDefaultAllocator(DWORD dwOutputStreamID, IUnknown* pAllocator)
{
	RETURN_HR_IF_NULL(E_POINTER, pAllocator);
	auto lock = _lock.lock_exclusive();
	auto index = GetStreamIndexById(dwOutputStreamID);
	RETURN_HR_IF(E_FAIL, index < 0);
	RETURN_HR_IF_MSG(E_FAIL, index < 0 || (DWORD)index >= _streams.size(), "dwOutputStreamID %u is invalid, index:%i", dwOutputStreamID, index);
	RETURN_HR(_streams[index]->SetAllocator(pAllocator));
}

STDMETHODIMP MFSource::GetAllocatorUsage(DWORD dwOutputStreamID, DWORD* pdwInputStreamID, MFSampleAllocatorUsage* peUsage)
{
	RETURN_HR_IF_NULL(E_POINTER, peUsage);
	RETURN_HR_IF_NULL(E_POINTER, pdwInputStreamID);
	auto lock = _lock.lock_exclusive();
	auto index = GetStreamIndexById(dwOutputStreamID);
	RETURN_HR_IF(E_FAIL, index < 0);
	RETURN_HR_IF_MSG(E_FAIL, index < 0 || (DWORD)index >= _streams.size(), "dwOutputStreamID %u is invalid, index:%i", dwOutputStreamID, index);
	*pdwInputStreamID = dwOutputStreamID;
	*peUsage = _streams[index]->GetAllocatorUsage();
	return S_OK;
}

STDMETHODIMP_(NTSTATUS) MFSource::KsProperty(PKSPROPERTY property, ULONG length, LPVOID data, ULONG dataLength, ULONG* bytesReturned)
{
	RETURN_HR_IF_NULL(E_POINTER, property);
	RETURN_HR_IF_NULL(E_POINTER, bytesReturned);
	auto lock = _lock.lock_exclusive();
	return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) MFSource::KsMethod(PKSMETHOD method, ULONG length, LPVOID data, ULONG dataLength, ULONG* bytesReturned)
{
	RETURN_HR_IF_NULL(E_POINTER, method);
	RETURN_HR_IF_NULL(E_POINTER, bytesReturned);
	auto lock = _lock.lock_exclusive();
	return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) MFSource::KsEvent(PKSEVENT evt, ULONG length, LPVOID data, ULONG dataLength, ULONG* bytesReturned)
{
	RETURN_HR_IF_NULL(E_POINTER, bytesReturned);
	auto lock = _lock.lock_exclusive();
	return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

HRESULT MFStream::Initialize(IMFMediaSource* source, int index)
{
	RETURN_HR_IF_NULL(E_POINTER, source);
	_source = source;
	_index = index;
    
    _initialWidth = 1280;
    _initialHeight = 720;
    
	RETURN_IF_FAILED(SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
	RETURN_IF_FAILED(SetUINT32(MF_DEVICESTREAM_STREAM_ID, index));
	RETURN_IF_FAILED(SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
	RETURN_IF_FAILED(SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes::MFFrameSourceTypes_Color));

	RETURN_IF_FAILED(MFCreateEventQueue(&_queue));

    std::vector<wil::com_ptr_nothrow<IMFMediaType>> mediaTypes;
    for (const auto& res : g_supportedResolutions)
    {
        for (const auto& fr : g_supportedFrameRates)
        {
            {
                wil::com_ptr_nothrow<IMFMediaType> rgbType;
                RETURN_IF_FAILED(MFCreateMediaType(&rgbType));
                rgbType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                rgbType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
                MFSetAttributeSize(rgbType.get(), MF_MT_FRAME_SIZE, res.width, res.height);
                rgbType->SetUINT32(MF_MT_DEFAULT_STRIDE, res.width * 4);
                rgbType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                rgbType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
                MFSetAttributeRatio(rgbType.get(), MF_MT_FRAME_RATE, fr.numerator, fr.denominator);
                auto bitrate = (uint32_t)(res.width * res.height * 4 * 8 * (fr.numerator / (float)fr.denominator));
                rgbType->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
                MFSetAttributeRatio(rgbType.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
                mediaTypes.push_back(rgbType);
            }

            {
                wil::com_ptr_nothrow<IMFMediaType> nv12Type;
                RETURN_IF_FAILED(MFCreateMediaType(&nv12Type));
                nv12Type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                nv12Type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
                MFSetAttributeSize(nv12Type.get(), MF_MT_FRAME_SIZE, res.width, res.height);
                nv12Type->SetUINT32(MF_MT_DEFAULT_STRIDE, res.width);
                nv12Type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                nv12Type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
                MFSetAttributeRatio(nv12Type.get(), MF_MT_FRAME_RATE, fr.numerator, fr.denominator);
                auto bitrate = (uint32_t)((res.width * res.height * 12 / 8) * (fr.numerator / (float)fr.denominator));
                nv12Type->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
                MFSetAttributeRatio(nv12Type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
                mediaTypes.push_back(nv12Type);
            }
        }
    }
    
    std::vector<IMFMediaType*> rawMediaTypes;
    rawMediaTypes.reserve(mediaTypes.size());
    for(const auto& mt : mediaTypes)
    {
        rawMediaTypes.push_back(mt.get());
    }

	RETURN_IF_FAILED_MSG(MFCreateStreamDescriptor(_index, (DWORD)rawMediaTypes.size(), rawMediaTypes.data(), &_descriptor), "MFCreateStreamDescriptor failed");

	wil::com_ptr_nothrow<IMFMediaTypeHandler> handler;
	RETURN_IF_FAILED(_descriptor->GetMediaTypeHandler(&handler));
	RETURN_IF_FAILED(handler->SetCurrentMediaType(mediaTypes[0].get()));

	return S_OK;
}

HRESULT MFStream::Start(IMFMediaType* type)
{
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue || !_allocator);

	if (type)
	{
		RETURN_IF_FAILED(type->GetGUID(MF_MT_SUBTYPE, &_format));
        UINT32 width, height;
        if (SUCCEEDED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height)))
        {
            if (width != _currentWidth || height != _currentHeight)
            {
                RETURN_IF_FAILED(_brokerClient.ReconfigureFormat(width, height));
                _currentWidth = width;
                _currentHeight = height;
            }
        }
	}

	if (!_brokerClient.HasD3DManager())
	{
        return E_UNEXPECTED;
	}

	RETURN_IF_FAILED(_allocator->InitializeSampleAllocator(10, type));
	RETURN_IF_FAILED(_queue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr));
	_state = MF_STREAM_STATE_RUNNING;
	return S_OK;
}

HRESULT MFStream::Stop()
{
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue || !_allocator);

	RETURN_IF_FAILED(_allocator->UninitializeSampleAllocator());
	RETURN_IF_FAILED(_queue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr));
	_state = MF_STREAM_STATE_STOPPED;
	return S_OK;
}

MFSampleAllocatorUsage MFStream::GetAllocatorUsage()
{
	return MFSampleAllocatorUsage_UsesProvidedAllocator;
}

HRESULT MFStream::SetAllocator(IUnknown* allocator)
{
	RETURN_HR_IF_NULL(E_POINTER, allocator);
	_allocator.reset();
	RETURN_HR(allocator->QueryInterface(&_allocator));
}

HRESULT MFStream::SetD3DManager(IUnknown* manager)
{
	RETURN_HR_IF_NULL(E_POINTER, manager);
	RETURN_IF_FAILED(_allocator->SetDirectXManager(manager));
	RETURN_IF_FAILED(_brokerClient.SetD3DManager(manager, _initialWidth, _initialHeight));
    _currentWidth = _initialWidth;
    _currentHeight = _initialHeight;
	return S_OK;
}

void MFStream::Shutdown()
{
	if (_queue)
	{
		LOG_IF_FAILED_MSG(_queue->Shutdown(), "Queue shutdown failed");
		_queue.reset();
	}
	_descriptor.reset();
	_source.reset();
	_attributes.reset();
}

STDMETHODIMP MFStream::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState)
{
	auto lock = _lock.lock_exclusive();
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);
	RETURN_IF_FAILED(_queue->BeginGetEvent(pCallback, punkState));
	return S_OK;
}

STDMETHODIMP MFStream::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
	RETURN_HR_IF_NULL(E_POINTER, ppEvent);
	*ppEvent = nullptr;
	auto lock = _lock.lock_exclusive();
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);
	RETURN_IF_FAILED(_queue->EndGetEvent(pResult, ppEvent));
	return S_OK;
}

STDMETHODIMP MFStream::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
	RETURN_HR_IF_NULL(E_POINTER, ppEvent);
	*ppEvent = nullptr;
	auto lock = _lock.lock_exclusive();
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);
	RETURN_IF_FAILED(_queue->GetEvent(dwFlags, ppEvent));
	return S_OK;
}

STDMETHODIMP MFStream::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue)
{
	auto lock = _lock.lock_exclusive();
	RETURN_HR_IF(MF_E_SHUTDOWN, !_queue);
	RETURN_IF_FAILED(_queue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue));
	return S_OK;
}

STDMETHODIMP MFStream::GetMediaSource(IMFMediaSource** ppMediaSource)
{
	RETURN_HR_IF_NULL(E_POINTER, ppMediaSource);
	*ppMediaSource = nullptr;
	RETURN_HR_IF(MF_E_SHUTDOWN, !_source);
	RETURN_IF_FAILED(_source.copy_to(ppMediaSource));
	return S_OK;
}

STDMETHODIMP MFStream::GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor)
{
	RETURN_HR_IF_NULL(E_POINTER, ppStreamDescriptor);
	*ppStreamDescriptor = nullptr;
	auto lock = _lock.lock_exclusive();
	RETURN_HR_IF(MF_E_SHUTDOWN, !_descriptor);
	RETURN_IF_FAILED(_descriptor.copy_to(ppStreamDescriptor));
	return S_OK;
}

STDMETHODIMP MFStream::RequestSample(IUnknown* pToken)
{
	auto lock = _lock.lock_exclusive();
	RETURN_HR_IF(MF_E_SHUTDOWN, !_allocator || !_queue);

	wil::com_ptr_nothrow<IMFSample> sample;
	RETURN_IF_FAILED(_allocator->AllocateSample(&sample));
	RETURN_IF_FAILED(sample->SetSampleTime(MFGetSystemTime()));
	RETURN_IF_FAILED(sample->SetSampleDuration(333333));

	wil::com_ptr_nothrow<IMFSample> outSample;
	RETURN_IF_FAILED(_brokerClient.Generate(sample.get(), _format, &outSample));

    if (!outSample)
    {
        return S_OK;
    }

	if (pToken)
	{
		RETURN_IF_FAILED(outSample->SetUnknown(MFSampleExtension_Token, pToken));
	}
	RETURN_IF_FAILED(_queue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, outSample.get()));
	return S_OK;
}

STDMETHODIMP MFStream::SetStreamState(MF_STREAM_STATE value)
{
	if (_state == value)
		return S_OK;

	switch (value)
	{
	case MF_STREAM_STATE_PAUSED:
		if (_state != MF_STREAM_STATE_RUNNING)
			RETURN_HR(MF_E_INVALID_STATE_TRANSITION);
		_state = value;
		break;

	case MF_STREAM_STATE_RUNNING:
		RETURN_IF_FAILED(Start(nullptr));
		break;

	case MF_STREAM_STATE_STOPPED:
		RETURN_IF_FAILED(Stop());
		break;

	default:
		RETURN_HR(MF_E_INVALID_STATE_TRANSITION);
		break;
	}
	return S_OK;
}

STDMETHODIMP MFStream::GetStreamState(MF_STREAM_STATE* value)
{
	RETURN_HR_IF_NULL(E_POINTER, value);
	*value = _state;
	return S_OK;
}

STDMETHODIMP_(NTSTATUS) MFStream::KsProperty(PKSPROPERTY property, ULONG length, LPVOID data, ULONG dataLength, ULONG* bytesReturned)
{
	RETURN_HR_IF_NULL(E_POINTER, property);
	RETURN_HR_IF_NULL(E_POINTER, bytesReturned);
	return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) MFStream::KsMethod(PKSMETHOD method, ULONG length, LPVOID data, ULONG dataLength, ULONG* bytesReturned)
{
	RETURN_HR_IF_NULL(E_POINTER, method);
	RETURN_HR_IF_NULL(E_POINTER, bytesReturned);
	return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP_(NTSTATUS) MFStream::KsEvent(PKSEVENT evt, ULONG length, LPVOID data, ULONG dataLength, ULONG* bytesReturned)
{
	RETURN_HR_IF_NULL(E_POINTER, bytesReturned);
	return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
	switch (dwReason)
	{
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(hModule);
		break;
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}

struct ClassFactory final : IClassFactory
{
	ClassFactory() { g_moduleObjectCount++; }
	virtual ~ClassFactory() { g_moduleObjectCount--; }

	STDMETHODIMP QueryInterface(REFIID riid, void** ppv)
	{
		RETURN_HR_IF_NULL(E_POINTER, ppv);
		*ppv = nullptr;
		if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory))
			*ppv = static_cast<IClassFactory*>(this);
		else
			return E_NOINTERFACE;
		AddRef();
		return S_OK;
	}

	STDMETHODIMP_(ULONG) AddRef() { return ++_refCount; }
	STDMETHODIMP_(ULONG) Release()
	{
		auto count = --_refCount;
		if (count == 0)
			delete this;
		return count;
	}

	STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** result) noexcept
	{
		RETURN_HR_IF_NULL(E_POINTER, result);
		*result = nullptr;
		if (outer)
			RETURN_HR(CLASS_E_NOAGGREGATION);

		wil::com_ptr_nothrow<MFActivate> vcam;
		try
		{
			vcam.attach(new MFActivate());
		}
		CATCH_RETURN();
		RETURN_IF_FAILED(vcam->Initialize());
		return vcam->QueryInterface(riid, result);
	}

	STDMETHODIMP LockServer(BOOL lock) noexcept
	{
		if (lock)
			g_moduleObjectCount++;
		else
			g_moduleObjectCount--;
		return S_OK;
	}

private:
	std::atomic<ULONG> _refCount{ 1 };
};

__control_entrypoint(DllExport)
STDAPI DllCanUnloadNow()
{
	return g_moduleObjectCount.load() == 0 ? S_OK : S_FALSE;
}

_Check_return_
STDAPI DllGetClassObject(_In_ REFCLSID rclsid, _In_ REFIID riid, _Outptr_ LPVOID FAR* ppv)
{
	RETURN_HR_IF_NULL(E_POINTER, ppv);
	*ppv = nullptr;

	if (IsEqualGUID(rclsid, CLSID_VCam))
	{
		wil::com_ptr_nothrow<ClassFactory> factory;
		factory.attach(new (std::nothrow) ClassFactory());
		RETURN_IF_NULL_ALLOC(factory);
		return factory->QueryInterface(riid, ppv);
	}

	RETURN_HR(CLASS_E_CLASSNOTAVAILABLE);
}

STDAPI DllRegisterServer()
{
    HMODULE hModule = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)DllRegisterServer, &hModule);

    std::wstring exePath = wil::GetModuleFileNameW(hModule).get();
	auto clsid = GUID_ToStringW(CLSID_VCam);
	std::wstring path = std::wstring(L"Software\\Classes\\CLSID\\") + clsid + L"\\InprocServer32";

	wil::unique_hkey key;
	RETURN_IF_WIN32_ERROR(RegWriteKey(HKEY_LOCAL_MACHINE, path.c_str(), key.put()));
	RETURN_IF_WIN32_ERROR(RegWriteValue(key.get(), nullptr, exePath));
	RETURN_IF_WIN32_ERROR(RegWriteValue(key.get(), L"ThreadingModel", L"Both"));
	return S_OK;
}

STDAPI DllUnregisterServer()
{
	auto clsid = GUID_ToStringW(CLSID_VCam);
	std::wstring path = std::wstring(L"Software\\Classes\\CLSID\\") + clsid;
	RETURN_IF_WIN32_ERROR(RegDeleteTree(HKEY_LOCAL_MACHINE, path.c_str()));
	return S_OK;
}