// =============================================================================
// CameraSource.cpp  --  Media Foundation virtual camera source
// =============================================================================

#include "CameraSource.h"
#include "Formats.h"

#include <mferror.h>
#include <ksmedia.h>
#include <propvarutil.h>
#include <vector>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::MakeAndInitialize;

namespace {

// Media type for one advertised (size, rate, subtype) combination.
HRESULT CreateVideoType(const Formats::Size& size, const Formats::Rate& rate, REFGUID subtype, IMFMediaType** result)
{
    wil::com_ptr_nothrow<IMFMediaType> type;
    RETURN_IF_FAILED(MFCreateMediaType(&type));
    RETURN_IF_FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    RETURN_IF_FAILED(type->SetGUID(MF_MT_SUBTYPE, subtype));
    RETURN_IF_FAILED(MFSetAttributeSize(type.get(), MF_MT_FRAME_SIZE, size.width, size.height));
    RETURN_IF_FAILED(MFSetAttributeRatio(type.get(), MF_MT_FRAME_RATE, rate.numerator, rate.denominator));
    RETURN_IF_FAILED(MFSetAttributeRatio(type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE));

    UINT32 sampleSize = 0;
    RETURN_IF_FAILED(MFCalculateImageSize(subtype, size.width, size.height, &sampleSize));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_SAMPLE_SIZE, sampleSize));
    LONG stride = 0;
    RETURN_IF_FAILED(MFGetStrideForBitmapInfoHeader(subtype.Data1, size.width, &stride));
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)std::abs(stride)));   // top-down
    RETURN_IF_FAILED(type->SetUINT32(MF_MT_AVG_BITRATE,
        (UINT32)std::min<UINT64>(0xFFFFFFFFull, (UINT64)sampleSize * 8 * rate.numerator / rate.denominator)));

    if (subtype == MFVideoFormat_RGB32) {
        RETURN_IF_FAILED(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_0_255));
    } else {
        RETURN_IF_FAILED(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235));
        RETURN_IF_FAILED(type->SetUINT32(MF_MT_YUV_MATRIX,
            size.height >= 720 ? MFVideoTransferMatrix_BT709 : MFVideoTransferMatrix_BT601));
    }
    *result = type.detach();
    return S_OK;
}

HRESULT QueueTimedEvent(IMFMediaEventQueue* queue, MediaEventType type)
{
    PROPVARIANT time;
    RETURN_IF_FAILED(InitPropVariantFromInt64(MFGetSystemTime(), &time));
    return queue->QueueEventParamVar(type, GUID_NULL, S_OK, &time);
}

} // namespace

// =============================================================================
// CameraStream
// =============================================================================

HRESULT CameraStream::RuntimeClassInitialize(CameraSource* source, DWORD streamId)
{
    m_source = source;
    m_id = streamId;
    RETURN_IF_FAILED(MFCreateEventQueue(&m_events));

    RETURN_IF_FAILED(MFCreateAttributes(&m_attributes, 4));
    RETURN_IF_FAILED(m_attributes->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
    RETURN_IF_FAILED(m_attributes->SetUINT32(MF_DEVICESTREAM_STREAM_ID, streamId));
    RETURN_IF_FAILED(m_attributes->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
    RETURN_IF_FAILED(m_attributes->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color));
    return BuildDescriptor();
}

HRESULT CameraStream::BuildDescriptor()
{
    static const GUID subtypes[] = { MFVideoFormat_NV12, MFVideoFormat_YUY2, MFVideoFormat_RGB32 };

    std::vector<wil::com_ptr_nothrow<IMFMediaType>> owned;
    std::vector<IMFMediaType*> types;
    for (const auto& size : Formats::kCameraSizes)
        for (const auto& rate : Formats::kCameraRates)
            for (const auto& subtype : subtypes) {
                wil::com_ptr_nothrow<IMFMediaType> type;
                RETURN_IF_FAILED(CreateVideoType(size, rate, subtype, &type));
                types.push_back(type.get());
                owned.push_back(std::move(type));
            }

    RETURN_IF_FAILED(MFCreateStreamDescriptor(m_id, (DWORD)types.size(), types.data(), &m_descriptor));
    wil::com_ptr_nothrow<IMFMediaTypeHandler> handler;
    RETURN_IF_FAILED(m_descriptor->GetMediaTypeHandler(&handler));
    RETURN_IF_FAILED(handler->SetCurrentMediaType(types.front()));
    return S_OK;
}

STDMETHODIMP CameraStream::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state)
{
    auto lock = m_lock.lock_shared();
    RETURN_HR_IF(MF_E_SHUTDOWN, m_shutdown);
    return m_events->BeginGetEvent(callback, state);
}

STDMETHODIMP CameraStream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event)
{
    auto lock = m_lock.lock_shared();
    RETURN_HR_IF(MF_E_SHUTDOWN, m_shutdown);
    return m_events->EndGetEvent(result, event);
}

STDMETHODIMP CameraStream::GetEvent(DWORD flags, IMFMediaEvent** event)
{
    // Blocking call: take a reference to the queue and wait outside the lock.
    wil::com_ptr_nothrow<IMFMediaEventQueue> queue;
    {
        auto lock = m_lock.lock_shared();
        RETURN_HR_IF(MF_E_SHUTDOWN, m_shutdown);
        queue = m_events;
    }
    return queue->GetEvent(flags, event);
}

STDMETHODIMP CameraStream::QueueEvent(MediaEventType type, REFGUID extended, HRESULT status, const PROPVARIANT* value)
{
    auto lock = m_lock.lock_shared();
    RETURN_HR_IF(MF_E_SHUTDOWN, m_shutdown);
    return m_events->QueueEventParamVar(type, extended, status, value);
}

STDMETHODIMP CameraStream::GetMediaSource(IMFMediaSource** source)
{
    RETURN_HR_IF_NULL(E_POINTER, source);
    *source = nullptr;
    auto lock = m_lock.lock_shared();
    RETURN_HR_IF(MF_E_SHUTDOWN, m_shutdown || !m_source);
    return m_source.copy_to(source);
}

STDMETHODIMP CameraStream::GetStreamDescriptor(IMFStreamDescriptor** descriptor)
{
    RETURN_HR_IF_NULL(E_POINTER, descriptor);
    *descriptor = nullptr;
    auto lock = m_lock.lock_shared();
    RETURN_HR_IF(MF_E_SHUTDOWN, m_shutdown);
    return m_descriptor.copy_to(descriptor);
}

STDMETHODIMP CameraStream::RequestSample(IUnknown* token)
{
    auto lock = m_lock.lock_exclusive();
    RETURN_HR_IF(MF_E_SHUTDOWN, m_shutdown);
    RETURN_HR_IF(MF_E_MEDIA_SOURCE_WRONGSTATE, m_state != MF_STREAM_STATE_RUNNING || !m_allocator);

    wil::com_ptr_nothrow<IMFSample> sample;
    RETURN_IF_FAILED(m_allocator->AllocateSample(&sample));
    RETURN_IF_FAILED(m_renderer.Render(sample.get()));
    RETURN_IF_FAILED(sample->SetSampleTime(MFGetSystemTime()));
    RETURN_IF_FAILED(sample->SetSampleDuration(m_frameDuration));
    if (token)
        RETURN_IF_FAILED(sample->SetUnknown(MFSampleExtension_Token, token));
    return m_events->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.get());
}

STDMETHODIMP CameraStream::SetStreamState(MF_STREAM_STATE state)
{
    {
        auto lock = m_lock.lock_exclusive();
        RETURN_HR_IF(MF_E_SHUTDOWN, m_shutdown);
        if (state == m_state)
            return S_OK;
        if (state == MF_STREAM_STATE_PAUSED) {
            RETURN_HR_IF(MF_E_INVALID_STATE_TRANSITION, m_state != MF_STREAM_STATE_RUNNING);
            m_state = state;
            return S_OK;
        }
    }
    if (state == MF_STREAM_STATE_RUNNING)
        return Start(nullptr);
    if (state == MF_STREAM_STATE_STOPPED)
        return Stop();
    return MF_E_INVALID_STATE_TRANSITION;
}

STDMETHODIMP CameraStream::GetStreamState(MF_STREAM_STATE* state)
{
    RETURN_HR_IF_NULL(E_POINTER, state);
    auto lock = m_lock.lock_shared();
    RETURN_HR_IF(MF_E_SHUTDOWN, m_shutdown);
    *state = m_state;
    return S_OK;
}

HRESULT CameraStream::ApplyMediaType(IMFMediaType* type)
{
    GUID subtype{};
    UINT32 width = 0, height = 0, numerator = 0, denominator = 0;
    RETURN_IF_FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype));
    RETURN_IF_FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height));
    if (FAILED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &numerator, &denominator)) || !numerator || !denominator) {
        numerator = 30;
        denominator = 1;
    }
    RETURN_IF_FAILED(m_renderer.Configure(width, height, subtype));
    m_frameDuration = Formats::FrameDuration(numerator, denominator);
    m_currentType = type;
    return S_OK;
}

HRESULT CameraStream::SetMediaType(IMFMediaType* type)
{
    RETURN_HR_IF_NULL(E_POINTER, type);
    auto lock = m_lock.lock_exclusive();
    RETURN_HR_IF(MF_E_SHUTDOWN, m_shutdown);
    wil::com_ptr_nothrow<IMFMediaTypeHandler> handler;
    RETURN_IF_FAILED(m_descriptor->GetMediaTypeHandler(&handler));
    RETURN_IF_FAILED(handler->IsMediaTypeSupported(type, nullptr));
    RETURN_IF_FAILED(handler->SetCurrentMediaType(type));
    return ApplyMediaType(type);
}

HRESULT CameraStream::Start(IMFMediaType* requested)
{
    auto lock = m_lock.lock_exclusive();
    RETURN_HR_IF(MF_E_SHUTDOWN, m_shutdown);

    wil::com_ptr_nothrow<IMFMediaType> type = requested;
    if (!type) {
        wil::com_ptr_nothrow<IMFMediaTypeHandler> handler;
        RETURN_IF_FAILED(m_descriptor->GetMediaTypeHandler(&handler));
        RETURN_IF_FAILED(handler->GetCurrentMediaType(&type));
    }
    RETURN_IF_FAILED(ApplyMediaType(type.get()));

    if (!m_allocator) {
        // The frame server normally provides one via SetDefaultAllocator.
        RETURN_IF_FAILED(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&m_allocator)));
        if (m_deviceManager)
            RETURN_IF_FAILED(m_allocator->SetDirectXManager(m_deviceManager.get()));
    }
    // Render-target binding lets the video processor write straight into the
    // allocator's textures.
    wil::com_ptr_nothrow<IMFAttributes> allocatorAttributes;
    RETURN_IF_FAILED(MFCreateAttributes(&allocatorAttributes, 1));
    RETURN_IF_FAILED(allocatorAttributes->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_RENDER_TARGET));
    m_allocator->UninitializeSampleAllocator();
    RETURN_IF_FAILED(m_allocator->InitializeSampleAllocatorEx(2, 8, allocatorAttributes.get(), type.get()));

    m_state = MF_STREAM_STATE_RUNNING;
    return m_events->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr);
}

HRESULT CameraStream::Stop()
{
    auto lock = m_lock.lock_exclusive();
    RETURN_HR_IF(MF_E_SHUTDOWN, m_shutdown);
    if (m_allocator)
        m_allocator->UninitializeSampleAllocator();
    m_state = MF_STREAM_STATE_STOPPED;
    return m_events->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

HRESULT CameraStream::SetAllocator(IUnknown* allocator)
{
    RETURN_HR_IF_NULL(E_POINTER, allocator);
    auto lock = m_lock.lock_exclusive();
    RETURN_HR_IF(MF_E_SHUTDOWN, m_shutdown);
    wil::com_ptr_nothrow<IMFVideoSampleAllocatorEx> sampleAllocator;
    RETURN_IF_FAILED(allocator->QueryInterface(IID_PPV_ARGS(&sampleAllocator)));
    if (m_deviceManager)
        RETURN_IF_FAILED(sampleAllocator->SetDirectXManager(m_deviceManager.get()));
    m_allocator = std::move(sampleAllocator);
    return S_OK;
}

HRESULT CameraStream::SetDeviceManager(IUnknown* manager)
{
    RETURN_HR_IF_NULL(E_POINTER, manager);
    auto lock = m_lock.lock_exclusive();
    RETURN_HR_IF(MF_E_SHUTDOWN, m_shutdown);
    RETURN_IF_FAILED(manager->QueryInterface(IID_PPV_ARGS(&m_deviceManager)));
    if (m_allocator)
        RETURN_IF_FAILED(m_allocator->SetDirectXManager(m_deviceManager.get()));
    return m_renderer.SetDeviceManager(m_deviceManager.get());
}

void CameraStream::Shutdown()
{
    auto lock = m_lock.lock_exclusive();
    if (m_shutdown)
        return;
    m_shutdown = true;
    m_state = MF_STREAM_STATE_STOPPED;
    m_events->Shutdown();
    if (m_allocator)
        m_allocator->UninitializeSampleAllocator();
    m_allocator.reset();
    m_renderer.Reset();
    m_deviceManager.reset();
    m_source.reset();
}

// =============================================================================
// CameraSource
// =============================================================================

HRESULT CameraSource::RuntimeClassInitialize(IMFAttributes* activationAttributes)
{
    RETURN_IF_FAILED(MFCreateAttributes(&m_attributes, 4));
    if (activationAttributes)
        RETURN_IF_FAILED(activationAttributes->CopyAllItems(m_attributes.get()));

    // Profiles let camera apps ask for their preferred class of mode.  None of
    // them restricts resolution or subtype; frame rate is capped only where
    // the profile's meaning requires it.
    wil::com_ptr_nothrow<IMFSensorProfileCollection> profiles;
    RETURN_IF_FAILED(MFCreateSensorProfileCollection(&profiles));
    const struct { REFGUID id; const wchar_t* filter; } kProfiles[] = {
        { KSCAMERAPROFILE_Legacy,            L"((RES==;FRT<=60,1;SUT==))" },
        { KSCAMERAPROFILE_VideoConferencing, L"((RES==;FRT<=60,1;SUT==))" },
        { KSCAMERAPROFILE_HighFrameRate,     L"((RES==;FRT>=60,1;SUT==))" },
    };
    for (const auto& p : kProfiles) {
        wil::com_ptr_nothrow<IMFSensorProfile> profile;
        RETURN_IF_FAILED(MFCreateSensorProfile(p.id, 0, nullptr, &profile));
        RETURN_IF_FAILED(profile->AddProfileFilter(0, p.filter));
        RETURN_IF_FAILED(profiles->AddProfile(profile.get()));
    }
    RETURN_IF_FAILED(m_attributes->SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, profiles.get()));

    RETURN_IF_FAILED(MakeAndInitialize<CameraStream>(&m_stream, this, 0));
    wil::com_ptr_nothrow<IMFStreamDescriptor> streamDescriptor;
    RETURN_IF_FAILED(m_stream->GetStreamDescriptor(&streamDescriptor));
    IMFStreamDescriptor* descriptors[] = { streamDescriptor.get() };
    RETURN_IF_FAILED(MFCreatePresentationDescriptor(1, descriptors, &m_presentation));
    RETURN_IF_FAILED(MFCreateEventQueue(&m_events));
    return S_OK;
}

STDMETHODIMP CameraSource::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state)
{
    auto lock = m_lock.lock_shared();
    RETURN_IF_FAILED(CheckAlive());
    return m_events->BeginGetEvent(callback, state);
}

STDMETHODIMP CameraSource::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event)
{
    auto lock = m_lock.lock_shared();
    RETURN_IF_FAILED(CheckAlive());
    return m_events->EndGetEvent(result, event);
}

STDMETHODIMP CameraSource::GetEvent(DWORD flags, IMFMediaEvent** event)
{
    wil::com_ptr_nothrow<IMFMediaEventQueue> queue;
    {
        auto lock = m_lock.lock_shared();
        RETURN_IF_FAILED(CheckAlive());
        queue = m_events;
    }
    return queue->GetEvent(flags, event);
}

STDMETHODIMP CameraSource::QueueEvent(MediaEventType type, REFGUID extended, HRESULT status, const PROPVARIANT* value)
{
    auto lock = m_lock.lock_shared();
    RETURN_IF_FAILED(CheckAlive());
    return m_events->QueueEventParamVar(type, extended, status, value);
}

STDMETHODIMP CameraSource::GetCharacteristics(DWORD* characteristics)
{
    RETURN_HR_IF_NULL(E_POINTER, characteristics);
    auto lock = m_lock.lock_shared();
    RETURN_IF_FAILED(CheckAlive());
    *characteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

STDMETHODIMP CameraSource::CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor)
{
    RETURN_HR_IF_NULL(E_POINTER, descriptor);
    *descriptor = nullptr;
    auto lock = m_lock.lock_shared();
    RETURN_IF_FAILED(CheckAlive());
    return m_presentation->Clone(descriptor);
}

STDMETHODIMP CameraSource::Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat, const PROPVARIANT* startPosition)
{
    RETURN_HR_IF_NULL(E_INVALIDARG, descriptor);
    RETURN_HR_IF_NULL(E_INVALIDARG, startPosition);
    RETURN_HR_IF(MF_E_UNSUPPORTED_TIME_FORMAT, timeFormat && *timeFormat != GUID_NULL);
    auto lock = m_lock.lock_exclusive();
    RETURN_IF_FAILED(CheckAlive());

    DWORD count = 0;
    RETURN_IF_FAILED(descriptor->GetStreamDescriptorCount(&count));
    for (DWORD i = 0; i < count; i++) {
        BOOL selected = FALSE;
        wil::com_ptr_nothrow<IMFStreamDescriptor> streamDescriptor;
        RETURN_IF_FAILED(descriptor->GetStreamDescriptorByIndex(i, &selected, &streamDescriptor));
        DWORD id = 0;
        RETURN_IF_FAILED(streamDescriptor->GetStreamIdentifier(&id));
        CameraStream* stream = StreamById(id);
        RETURN_HR_IF(MF_E_INVALIDSTREAMNUMBER, !stream);

        if (selected) {
            RETURN_IF_FAILED(m_presentation->SelectStream(i));
            wil::com_ptr_nothrow<IMFMediaTypeHandler> handler;
            wil::com_ptr_nothrow<IMFMediaType> type;
            RETURN_IF_FAILED(streamDescriptor->GetMediaTypeHandler(&handler));
            RETURN_IF_FAILED(handler->GetCurrentMediaType(&type));
            const MediaEventType announce = stream->IsActive() ? MEUpdatedStream : MENewStream;
            RETURN_IF_FAILED(m_events->QueueEventParamUnk(announce, GUID_NULL, S_OK, static_cast<IMFMediaStream2*>(stream)));
            RETURN_IF_FAILED(stream->Start(type.get()));
        } else if (stream->IsActive()) {
            RETURN_IF_FAILED(m_presentation->DeselectStream(i));
            RETURN_IF_FAILED(stream->Stop());
        }
    }
    return QueueTimedEvent(m_events.get(), MESourceStarted);
}

STDMETHODIMP CameraSource::Stop()
{
    auto lock = m_lock.lock_exclusive();
    RETURN_IF_FAILED(CheckAlive());
    if (m_stream->IsActive())
        RETURN_IF_FAILED(m_stream->Stop());
    return QueueTimedEvent(m_events.get(), MESourceStopped);
}

STDMETHODIMP CameraSource::Pause()
{
    // Live capture sources do not pause.
    return MF_E_INVALID_STATE_TRANSITION;
}

STDMETHODIMP CameraSource::Shutdown()
{
    auto lock = m_lock.lock_exclusive();
    RETURN_IF_FAILED(CheckAlive());
    m_events->Shutdown();
    m_events.reset();
    if (m_stream)
        m_stream->Shutdown();
    m_stream.Reset();
    m_presentation.reset();
    return S_OK;
}

STDMETHODIMP CameraSource::GetSourceAttributes(IMFAttributes** attributes)
{
    RETURN_HR_IF_NULL(E_POINTER, attributes);
    auto lock = m_lock.lock_shared();
    RETURN_IF_FAILED(CheckAlive());
    return m_attributes.copy_to(attributes);
}

STDMETHODIMP CameraSource::GetStreamAttributes(DWORD streamId, IMFAttributes** attributes)
{
    RETURN_HR_IF_NULL(E_POINTER, attributes);
    *attributes = nullptr;
    auto lock = m_lock.lock_shared();
    RETURN_IF_FAILED(CheckAlive());
    CameraStream* stream = StreamById(streamId);
    RETURN_HR_IF(MF_E_INVALIDSTREAMNUMBER, !stream);
    *attributes = stream->Attributes();
    (*attributes)->AddRef();
    return S_OK;
}

STDMETHODIMP CameraSource::SetD3DManager(IUnknown* manager)
{
    auto lock = m_lock.lock_shared();
    RETURN_IF_FAILED(CheckAlive());
    return m_stream->SetDeviceManager(manager);
}

STDMETHODIMP CameraSource::SetMediaType(DWORD streamId, IMFMediaType* type)
{
    auto lock = m_lock.lock_shared();
    RETURN_IF_FAILED(CheckAlive());
    CameraStream* stream = StreamById(streamId);
    RETURN_HR_IF(MF_E_INVALIDSTREAMNUMBER, !stream);
    return stream->SetMediaType(type);
}

STDMETHODIMP CameraSource::GetService(REFGUID, REFIID, LPVOID* object)
{
    if (object)
        *object = nullptr;
    return MF_E_UNSUPPORTED_SERVICE;
}

STDMETHODIMP CameraSource::SetDefaultAllocator(DWORD streamId, IUnknown* allocator)
{
    auto lock = m_lock.lock_shared();
    RETURN_IF_FAILED(CheckAlive());
    CameraStream* stream = StreamById(streamId);
    RETURN_HR_IF(MF_E_INVALIDSTREAMNUMBER, !stream);
    return stream->SetAllocator(allocator);
}

STDMETHODIMP CameraSource::GetAllocatorUsage(DWORD streamId, DWORD* inputStreamId, MFSampleAllocatorUsage* usage)
{
    RETURN_HR_IF_NULL(E_POINTER, inputStreamId);
    RETURN_HR_IF_NULL(E_POINTER, usage);
    auto lock = m_lock.lock_shared();
    RETURN_IF_FAILED(CheckAlive());
    RETURN_HR_IF(MF_E_INVALIDSTREAMNUMBER, !StreamById(streamId));
    *inputStreamId = streamId;
    *usage = MFSampleAllocatorUsage_UsesProvidedAllocator;
    return S_OK;
}

STDMETHODIMP CameraSource::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG* returned)
{
    if (returned) *returned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP CameraSource::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG* returned)
{
    if (returned) *returned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP CameraSource::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG* returned)
{
    if (returned) *returned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

// =============================================================================
// CameraActivate
// =============================================================================

HRESULT CameraActivate::RuntimeClassInitialize()
{
    return MFCreateAttributes(&m_attributes, 4);
}

STDMETHODIMP CameraActivate::ActivateObject(REFIID riid, void** object)
{
    RETURN_HR_IF_NULL(E_POINTER, object);
    *object = nullptr;
    auto lock = m_lock.lock();
    if (!m_source)
        RETURN_IF_FAILED(MakeAndInitialize<CameraSource>(&m_source, m_attributes.get()));
    return m_source.CopyTo(riid, object);
}

STDMETHODIMP CameraActivate::ShutdownObject()
{
    auto lock = m_lock.lock();
    if (m_source)
        m_source->Shutdown();
    m_source.Reset();
    return S_OK;
}

STDMETHODIMP CameraActivate::DetachObject()
{
    auto lock = m_lock.lock();
    m_source.Reset();
    return S_OK;
}
