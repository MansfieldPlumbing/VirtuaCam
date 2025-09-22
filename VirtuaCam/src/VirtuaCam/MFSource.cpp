#include "pch.h"
#include "Guids.h"
#include "MFSource.h"
#include "App.h"
#include "Formats.h"

MFSource::MFSource() : m_videoStreams(m_streamCount)
{
    THROW_IF_FAILED(MFCreateAttributes(&m_attributes, 0));
    for (auto i = 0; i < m_streamCount; i++)
    {
        auto stream = winrt::make_self<MFStream>();
        stream->Initialize(this, i);
        m_videoStreams[i] = stream;
    }
}

HRESULT MFSource::Initialize(IMFAttributes* attributes)
{
    if (attributes)
    {
        RETURN_IF_FAILED(attributes->CopyAllItems(m_attributes.get()));
    }
    
    m_pipeName = L"\\\\.\\pipe\\VirtuaCam_IPC_" + VirtuaCam::Utils::Debug::GuidToWString(CLSID_VirtuaCam, false);
    RETURN_IF_FAILED(m_attributes->SetString(MF_DEVSOURCE_ATTRIBUTE_NAMED_PIPE_NAME, m_pipeName.c_str()));

    wil::com_ptr_nothrow<IMFSensorProfileCollection> profileCollection;
    RETURN_IF_FAILED(MFCreateSensorProfileCollection(&profileCollection));
    DWORD streamId = 0;
    
    wil::com_ptr_nothrow<IMFSensorProfile> profile;
    RETURN_IF_FAILED(MFCreateSensorProfile(KSCAMERAPROFILE_VideoConferencing, 0, nullptr, &profile));
    RETURN_IF_FAILED(profile->AddProfileFilter(streamId, L"((FRT==30,1))"));
    RETURN_IF_FAILED(profileCollection->AddProfile(profile.get()));
    
    profile = nullptr;
    RETURN_IF_FAILED(MFCreateSensorProfile(KSCAMERAPROFILE_HighFrameRate, 0, nullptr, &profile));
    RETURN_IF_FAILED(profile->AddProfileFilter(streamId, L"((FRT>=60,1))"));
    RETURN_IF_FAILED(profileCollection->AddProfile(profile.get()));
    
    profile = nullptr;
    RETURN_IF_FAILED(MFCreateSensorProfile(KSCAMERAPROFILE_Legacy, 0, nullptr, &profile));
    RETURN_IF_FAILED(profile->AddProfileFilter(streamId, L"((FRT<60,1))"));
    RETURN_IF_FAILED(profileCollection->AddProfile(profile.get()));

    RETURN_IF_FAILED(SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, profileCollection.get()));

    auto streamDescriptors = VirtuaCam::Utils::make_unique_cotaskmem_array<wil::com_ptr_nothrow<IMFStreamDescriptor>>(m_videoStreams.size());
    for (uint32_t i = 0; i < streamDescriptors.size(); i++)
    {
        wil::com_ptr_nothrow<IMFStreamDescriptor> descriptor;
        RETURN_IF_FAILED(m_videoStreams[i]->GetStreamDescriptor(&descriptor));
        streamDescriptors[i] = descriptor.detach();
    }
    
    RETURN_IF_FAILED(MFCreatePresentationDescriptor((DWORD)streamDescriptors.size(), streamDescriptors.get(), &m_presentationDescriptor));
    RETURN_IF_FAILED(MFCreateEventQueue(&m_eventQueue));

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = FALSE;
    wil::unique_hlocal_security_descriptor sd;
    PSECURITY_DESCRIPTOR pSD = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)", SDDL_REVISION_1, &pSD, NULL))
    {
        sd.reset(pSD);
        sa.lpSecurityDescriptor = sd.get();
    }

    m_hControlsMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(VirtuaCamControls), VIRTUCAM_CONTROLS_SHM_NAME);
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(GetLastError()), m_hControlsMapping);
    m_pControlsView = (VirtuaCamControls*)MapViewOfFile(m_hControlsMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VirtuaCamControls));
    RETURN_HR_IF_NULL(HRESULT_FROM_WIN32(GetLastError()), m_pControlsView);
    *m_pControlsView = VirtuaCamControls();

    StartIpcServer();

    return S_OK;
}

int MFSource::GetStreamIndexById(DWORD id)
{
    for (uint32_t i = 0; i < m_videoStreams.size(); i++)
    {
        wil::com_ptr_nothrow<IMFStreamDescriptor> desc;
        if (FAILED(m_videoStreams[i]->GetStreamDescriptor(&desc)))
            return -1;
        
        DWORD streamId = 0;
        if (FAILED(desc->GetStreamIdentifier(&streamId)))
            return -1;
            
        if (streamId == id)
            return i;
    }
    return -1;
}

HRESULT MFSource::_QueueEvent(MediaEventType eventType, const PROPVARIANT* eventValue)
{
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_eventQueue);
    return m_eventQueue->QueueEventParamVar(eventType, GUID_NULL, S_OK, eventValue);
}

STDMETHODIMP MFSource::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState)
{
    winrt::slim_lock_guard lock(m_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_eventQueue);
    return m_eventQueue->BeginGetEvent(pCallback, punkState);
}

STDMETHODIMP MFSource::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
    RETURN_HR_IF_NULL(E_POINTER, ppEvent);
    *ppEvent = nullptr;
    winrt::slim_lock_guard lock(m_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_eventQueue);
    return m_eventQueue->EndGetEvent(pResult, ppEvent);
}

STDMETHODIMP MFSource::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
    RETURN_HR_IF_NULL(E_POINTER, ppEvent);
    *ppEvent = nullptr;
    winrt::slim_lock_guard lock(m_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_eventQueue);
    return m_eventQueue->GetEvent(dwFlags, ppEvent);
}

STDMETHODIMP MFSource::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue)
{
    winrt::slim_lock_guard lock(m_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_eventQueue);
    return m_eventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
}

STDMETHODIMP MFSource::CreatePresentationDescriptor(IMFPresentationDescriptor** ppPresentationDescriptor)
{
    RETURN_HR_IF_NULL(E_POINTER, ppPresentationDescriptor);
    *ppPresentationDescriptor = nullptr;
    winrt::slim_lock_guard lock(m_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_presentationDescriptor);
    return m_presentationDescriptor->Clone(ppPresentationDescriptor);
}

STDMETHODIMP MFSource::GetCharacteristics(DWORD* pdwCharacteristics)
{
    RETURN_HR_IF_NULL(E_POINTER, pdwCharacteristics);
    *pdwCharacteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

STDMETHODIMP MFSource::Pause()
{
    return MF_E_INVALID_STATE_TRANSITION;
}

STDMETHODIMP MFSource::Shutdown()
{
    winrt::slim_lock_guard lock(m_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_eventQueue);
    
    StopIpcServer();

    if (m_pControlsView) UnmapViewOfFile(m_pControlsView);
    if (m_hControlsMapping) CloseHandle(m_hControlsMapping);
    m_pControlsView = nullptr;
    m_hControlsMapping = nullptr;

    if (m_eventQueue)
    {
        m_eventQueue->Shutdown();
    }
    for (uint32_t i = 0; i < m_videoStreams.size(); i++)
    {
        if(m_videoStreams[i]) m_videoStreams[i]->Shutdown();
    }

    m_eventQueue.reset();
    m_presentationDescriptor.reset();
    m_attributes.reset();
    return S_OK;
}

STDMETHODIMP MFSource::Start(IMFPresentationDescriptor* pPresentationDescriptor, const GUID* pguidTimeFormat, const PROPVARIANT* pvarStartPosition)
{
    RETURN_HR_IF_NULL(E_POINTER, pPresentationDescriptor);
    RETURN_HR_IF_NULL(E_POINTER, pvarStartPosition);
    RETURN_HR_IF_MSG(E_INVALIDARG, pguidTimeFormat && *pguidTimeFormat != GUID_NULL, "Unsupported time format");
    
    winrt::slim_lock_guard lock(m_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_eventQueue || !m_presentationDescriptor);

    DWORD descriptorCount;
    RETURN_IF_FAILED(pPresentationDescriptor->GetStreamDescriptorCount(&descriptorCount));
    RETURN_HR_IF_MSG(E_INVALIDARG, descriptorCount != (DWORD)m_videoStreams.size(), "Invalid stream descriptor count");

    for (DWORD i = 0; i < descriptorCount; i++)
    {
        wil::com_ptr_nothrow<IMFStreamDescriptor> descriptor;
        BOOL isSelected = FALSE;
        RETURN_IF_FAILED(pPresentationDescriptor->GetStreamDescriptorByIndex(i, &isSelected, &descriptor));

        DWORD streamId = 0;
        RETURN_IF_FAILED(descriptor->GetStreamIdentifier(&streamId));
        auto streamIndex = GetStreamIndexById(streamId);
        RETURN_HR_IF(E_FAIL, streamIndex < 0);

        if (isSelected)
        {
            RETURN_IF_FAILED(m_presentationDescriptor->SelectStream(streamIndex));
            m_eventQueue->QueueEventParamUnk(MENewStream, GUID_NULL, S_OK, m_videoStreams[streamIndex].as<IUnknown>().get());

            wil::com_ptr_nothrow<IMFMediaTypeHandler> mediaTypeHandler;
            wil::com_ptr_nothrow<IMFMediaType> mediaType;
            RETURN_IF_FAILED(descriptor->GetMediaTypeHandler(&mediaTypeHandler));
            RETURN_IF_FAILED(mediaTypeHandler->GetCurrentMediaType(&mediaType));

            RETURN_IF_FAILED(m_videoStreams[streamIndex]->Start(mediaType.get()));
        }
        else
        {
            RETURN_IF_FAILED(m_presentationDescriptor->DeselectStream(streamIndex));
            RETURN_IF_FAILED(m_videoStreams[streamIndex]->Stop());
        }
    }

    wil::unique_prop_variant time;
    RETURN_IF_FAILED(InitPropVariantFromInt64(MFGetSystemTime(), &time));
    RETURN_IF_FAILED(_QueueEvent(MESourceStarted, &time));
    return S_OK;
}

STDMETHODIMP MFSource::Stop()
{
    winrt::slim_lock_guard lock(m_lock);
    RETURN_HR_IF(MF_E_SHUTDOWN, !m_eventQueue || !m_presentationDescriptor);

    for (DWORD i = 0; i < m_videoStreams.size(); i++)
    {
        RETURN_IF_FAILED(m_videoStreams[i]->Stop());
        RETURN_IF_FAILED(m_presentationDescriptor->DeselectStream(i));
    }
    
    wil::unique_prop_variant time;
    RETURN_IF_FAILED(InitPropVariantFromInt64(MFGetSystemTime(), &time));
    RETURN_IF_FAILED(_QueueEvent(MESourceStopped, &time));
    return S_OK;
}

STDMETHODIMP MFSource::GetSourceAttributes(IMFAttributes** ppAttributes)
{
    RETURN_HR_IF_NULL(E_POINTER, ppAttributes);
    winrt::slim_lock_guard lock(m_lock);
    return this->QueryInterface(__uuidof(IMFAttributes), reinterpret_cast<void**>(ppAttributes));
}

STDMETHODIMP MFSource::SetMediaType(DWORD, IMFMediaType*)
{
    return S_OK;
}

STDMETHODIMP MFSource::GetStreamAttributes(DWORD dwStreamIdentifier, IMFAttributes** ppAttributes)
{
    RETURN_HR_IF_NULL(E_POINTER, ppAttributes);
    *ppAttributes = nullptr;
    winrt::slim_lock_guard lock(m_lock);
    RETURN_HR_IF(E_INVALIDARG, dwStreamIdentifier >= m_videoStreams.size());
    return m_videoStreams[dwStreamIdentifier]->QueryInterface(IID_PPV_ARGS(ppAttributes));
}

STDMETHODIMP MFSource::SetD3DManager(IUnknown* pManager)
{
    RETURN_HR_IF_NULL(E_POINTER, pManager);
    winrt::slim_lock_guard lock(m_lock);
    for (DWORD i = 0; i < m_videoStreams.size(); i++)
    {
        RETURN_IF_FAILED(m_videoStreams[i]->SetD3DManager(pManager));
    }
    return S_OK;
}

STDMETHODIMP MFSource::GetService(REFGUID guidService, REFIID riid, LPVOID* ppvObject)
{
    if (!ppvObject)
    {
        return E_POINTER;
    }
    *ppvObject = nullptr;

    return MF_E_UNSUPPORTED_SERVICE;
}

STDMETHODIMP MFSource::SetDefaultAllocator(DWORD dwOutputStreamID, IUnknown* pAllocator)
{
    RETURN_HR_IF_NULL(E_POINTER, pAllocator);
    winrt::slim_lock_guard lock(m_lock);
    auto streamIndex = GetStreamIndexById(dwOutputStreamID);
    RETURN_HR_IF(E_INVALIDARG, streamIndex < 0);
    return m_videoStreams[streamIndex]->SetAllocator(pAllocator);
}

STDMETHODIMP MFSource::GetAllocatorUsage(DWORD dwOutputStreamID, DWORD* pdwInputStreamID, MFSampleAllocatorUsage* peUsage)
{
    RETURN_HR_IF_NULL(E_POINTER, peUsage);
    RETURN_HR_IF_NULL(E_POINTER, pdwInputStreamID);
    winrt::slim_lock_guard lock(m_lock);
    auto streamIndex = GetStreamIndexById(dwOutputStreamID);
    RETURN_HR_IF(E_INVALIDARG, streamIndex < 0);
    *pdwInputStreamID = dwOutputStreamID;
    *peUsage = m_videoStreams[streamIndex]->GetAllocatorUsage();
    return S_OK;
}

STDMETHODIMP_(NTSTATUS) MFSource::KsProperty(PKSPROPERTY Property, ULONG PropertyLength, LPVOID PropertyData, ULONG DataLength, ULONG* pBytesReturned)
{
    winrt::slim_lock_guard lock(m_lock);
    *pBytesReturned = 0;

    if (Property->Set != PROPSETID_VIDCAP_VIDEOPROCAMP)
    {
        return STATUS_NOT_FOUND;
    }

    KSPROPERTY_VIDEOPROCAMP_S* procAmp = (KSPROPERTY_VIDEOPROCAMP_S*)PropertyData;
    long* value = nullptr;
    long minVal = 0, maxVal = 200, step = 1, defaultVal = 100;

    switch (Property->Id)
    {
    case KSPROPERTY_VIDEOPROCAMP_BRIGHTNESS:
        value = &m_pControlsView->brightness;
        minVal = -100; maxVal = 100; step = 1; defaultVal = 0;
        break;
    case KSPROPERTY_VIDEOPROCAMP_CONTRAST:
        value = &m_pControlsView->contrast;
        minVal = 0; maxVal = 200; step = 1; defaultVal = 100;
        break;
    case KSPROPERTY_VIDEOPROCAMP_SATURATION:
        value = &m_pControlsView->saturation;
        minVal = 0; maxVal = 200; step = 1; defaultVal = 100;
        break;
    default:
        return STATUS_NOT_FOUND;
    }

    if (Property->Flags & KSPROPERTY_TYPE_GET)
    {
        if (DataLength < sizeof(KSPROPERTY_VIDEOPROCAMP_S)) return STATUS_BUFFER_TOO_SMALL;
        procAmp->Value = *value;
        procAmp->Flags = KSPROPERTY_VIDEOPROCAMP_FLAGS_MANUAL;
        procAmp->Capabilities = KSPROPERTY_VIDEOPROCAMP_FLAGS_MANUAL;
        *pBytesReturned = sizeof(KSPROPERTY_VIDEOPROCAMP_S);
    }
    else if (Property->Flags & KSPROPERTY_TYPE_SET)
    {
        if (DataLength < sizeof(KSPROPERTY_VIDEOPROCAMP_S)) return STATUS_BUFFER_TOO_SMALL;
        *value = procAmp->Value;
        *pBytesReturned = 0;
    }
    else if (Property->Flags & KSPROPERTY_TYPE_BASICSUPPORT)
    {
        if (DataLength < sizeof(KSPROPERTY_DESCRIPTION)) return STATUS_BUFFER_TOO_SMALL;
        KSPROPERTY_DESCRIPTION* desc = (KSPROPERTY_DESCRIPTION*)PropertyData;
        desc->AccessFlags = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT;
        desc->DescriptionSize = sizeof(KSPROPERTY_DESCRIPTION) + sizeof(KSPROPERTY_MEMBERSHEADER) + sizeof(KSPROPERTY_STEPPING_LONG);
        desc->PropTypeSet.Set = KSPROPTYPESETID_General;
        desc->PropTypeSet.Id = VT_I4;
        desc->PropTypeSet.Flags = 0;
        desc->MembersListCount = 1;
        
        if(DataLength < desc->DescriptionSize) {
             *pBytesReturned = desc->DescriptionSize;
             return STATUS_BUFFER_OVERFLOW;
        }

        KSPROPERTY_MEMBERSHEADER* header = (KSPROPERTY_MEMBERSHEADER*)(desc + 1);
        header->MembersFlags = KSPROPERTY_MEMBER_VALUES;
        header->MembersSize = sizeof(KSPROPERTY_STEPPING_LONG);
        header->MembersCount = 1;
        header->Flags = 0;

        KSPROPERTY_STEPPING_LONG* stepping = (KSPROPERTY_STEPPING_LONG*)(header + 1);
        stepping->SteppingDelta = step;
        stepping->Bounds.SignedMinimum = minVal;
        stepping->Bounds.SignedMaximum = maxVal;
        
        *pBytesReturned = desc->DescriptionSize;
    }
    else
    {
        return STATUS_NOT_IMPLEMENTED;
    }

    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) MFSource::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG* pBytesReturned)
{
    if (pBytesReturned != nullptr)
    {
        *pBytesReturned = 0;
    }
    return STATUS_NOT_IMPLEMENTED;
}

STDMETHODIMP_(NTSTATUS) MFSource::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG* pBytesReturned)
{
    if (pBytesReturned != nullptr)
    {
        *pBytesReturned = 0;
    }
    return STATUS_NOT_IMPLEMENTED;
}


STDMETHODIMP MFSource::Compare(IMFAttributes* pTheirs, MF_ATTRIBUTES_MATCH_TYPE MatchType, BOOL* pbResult) { return m_attributes->Compare(pTheirs, MatchType, pbResult); }
STDMETHODIMP MFSource::CompareItem(REFGUID guidKey, REFPROPVARIANT Value, BOOL* pbResult) { return m_attributes->CompareItem(guidKey, Value, pbResult); }
STDMETHODIMP MFSource::CopyAllItems(IMFAttributes* pDest) { return m_attributes->CopyAllItems(pDest); }
STDMETHODIMP MFSource::DeleteAllItems() { return m_attributes->DeleteAllItems(); }
STDMETHODIMP MFSource::DeleteItem(REFGUID guidKey) { return m_attributes->DeleteItem(guidKey); }
STDMETHODIMP MFSource::GetAllocatedBlob(REFGUID guidKey, UINT8** ppBuf, UINT32* pcbSize) { return m_attributes->GetAllocatedBlob(guidKey, ppBuf, pcbSize); }
STDMETHODIMP MFSource::GetAllocatedString(REFGUID guidKey, LPWSTR* ppwszValue, UINT32* pcchLength) { return m_attributes->GetAllocatedString(guidKey, ppwszValue, pcchLength); }
STDMETHODIMP MFSource::GetBlob(REFGUID guidKey, UINT8* pBuf, UINT32 cbBufSize, UINT32* pcbBlobSize) { return m_attributes->GetBlob(guidKey, pBuf, cbBufSize, pcbBlobSize); }
STDMETHODIMP MFSource::GetBlobSize(REFGUID guidKey, UINT32* pcbBlobSize) { return m_attributes->GetBlobSize(guidKey, pcbBlobSize); }
STDMETHODIMP MFSource::GetCount(UINT32* pcItems) { return m_attributes->GetCount(pcItems); }
STDMETHODIMP MFSource::GetDouble(REFGUID guidKey, double* pfValue) { return m_attributes->GetDouble(guidKey, pfValue); }
STDMETHODIMP MFSource::GetGUID(REFGUID guidKey, GUID* pguidValue) { return m_attributes->GetGUID(guidKey, pguidValue); }
STDMETHODIMP MFSource::GetItem(REFGUID guidKey, PROPVARIANT* pValue) { return m_attributes->GetItem(guidKey, pValue); }
STDMETHODIMP MFSource::GetItemByIndex(UINT32 unIndex, GUID* pguidKey, PROPVARIANT* pValue) { return m_attributes->GetItemByIndex(unIndex, pguidKey, pValue); }
STDMETHODIMP MFSource::GetItemType(REFGUID guidKey, MF_ATTRIBUTE_TYPE* pType) { return m_attributes->GetItemType(guidKey, pType); }
STDMETHODIMP MFSource::GetString(REFGUID guidKey, LPWSTR pwszValue, UINT32 cchBufSize, UINT32* pcchLength) { return m_attributes->GetString(guidKey, pwszValue, cchBufSize, pcchLength); }
STDMETHODIMP MFSource::GetStringLength(REFGUID guidKey, UINT32* pcchLength) { return m_attributes->GetStringLength(guidKey, pcchLength); }
STDMETHODIMP MFSource::GetUINT32(REFGUID guidKey, UINT32* punValue) { return m_attributes->GetUINT32(guidKey, punValue); }
STDMETHODIMP MFSource::GetUINT64(REFGUID guidKey, UINT64* punValue) { return m_attributes->GetUINT64(guidKey, punValue); }
STDMETHODIMP MFSource::GetUnknown(REFGUID guidKey, REFIID riid, LPVOID* ppv) { return m_attributes->GetUnknown(guidKey, riid, ppv); }
STDMETHODIMP MFSource::LockStore() { return m_attributes->LockStore(); }
STDMETHODIMP MFSource::SetBlob(REFGUID guidKey, const UINT8* pBuf, UINT32 cbBufSize) { return m_attributes->SetBlob(guidKey, pBuf, cbBufSize); }
STDMETHODIMP MFSource::SetDouble(REFGUID guidKey, double fValue) { return m_attributes->SetDouble(guidKey, fValue); }
STDMETHODIMP MFSource::SetGUID(REFGUID guidKey, REFGUID guidValue) { return m_attributes->SetGUID(guidKey, guidValue); }
STDMETHODIMP MFSource::SetItem(REFGUID guidKey, REFPROPVARIANT Value) { return m_attributes->SetItem(guidKey, Value); }
STDMETHODIMP MFSource::SetString(REFGUID guidKey, LPCWSTR pszValue) { return m_attributes->SetString(guidKey, pszValue); }
STDMETHODIMP MFSource::SetUINT32(REFGUID guidKey, UINT32 unValue) { return m_attributes->SetUINT32(guidKey, unValue); }
STDMETHODIMP MFSource::SetUINT64(REFGUID guidKey, UINT64 unValue) { return m_attributes->SetUINT64(guidKey, unValue); }
STDMETHODIMP MFSource::SetUnknown(REFGUID guidKey, IUnknown* pUnknown) { return m_attributes->SetUnknown(guidKey, pUnknown); }
STDMETHODIMP MFSource::UnlockStore() { return m_attributes->UnlockStore(); }

void MFSource::StartIpcServer()
{
    m_ipcShutdownEvent.create();
    m_hIpcThread = CreateThread(nullptr, 0, IpcThread, this, 0, nullptr);
}

void MFSource::StopIpcServer()
{
    if (m_ipcShutdownEvent)
    {
        m_ipcShutdownEvent.SetEvent();
    }

    if (m_hIpcThread)
    {
        if (m_hIpcPipe != INVALID_HANDLE_VALUE)
        {
            HANDLE hDummyClient = CreateFileW(m_pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            if (hDummyClient != INVALID_HANDLE_VALUE)
            {
                CloseHandle(hDummyClient);
            }
        }
        
        WaitForSingleObject(m_hIpcThread, 2000);
        CloseHandle(m_hIpcThread);
        m_hIpcThread = nullptr;
    }

    if (m_hIpcPipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_hIpcPipe);
        m_hIpcPipe = INVALID_HANDLE_VALUE;
    }
}


DWORD WINAPI MFSource::IpcThread(LPVOID lpParam)
{
    MFSource* pThis = static_cast<MFSource*>(lpParam);
    
    pThis->m_hIpcPipe = CreateNamedPipeW(
        pThis->m_pipeName.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        sizeof(DWORD), sizeof(DWORD), 0, nullptr);

    if (pThis->m_hIpcPipe == INVALID_HANDLE_VALUE)
    {
        return 1;
    }

    while (WaitForSingleObject(pThis->m_ipcShutdownEvent.get(), 0) != WAIT_OBJECT_0)
    {
        BOOL connected = ConnectNamedPipe(pThis->m_hIpcPipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected)
        {
            DWORD command = 0;
            DWORD bytesRead = 0;
            if (ReadFile(pThis->m_hIpcPipe, &command, sizeof(DWORD), &bytesRead, nullptr) && bytesRead == sizeof(DWORD))
            {
                pThis->HandleIpcCommand(command);
            }
            DisconnectNamedPipe(pThis->m_hIpcPipe);
        }
        else
        {
            if (GetLastError() != ERROR_PIPE_LISTENING)
            {
                 Sleep(50);
            }
        }
    }
    
    return 0;
}

void MFSource::HandleIpcCommand(DWORD command)
{
    switch (static_cast<VirtuaCamCommand>(command))
    {
    case VirtuaCamCommand::None:
        break;
    }
}