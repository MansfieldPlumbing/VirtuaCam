#include "pch.h"
#include "WASAPI.h"
#include "App.h"
#include <propkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#pragma comment(lib, "avrt.lib")

WASAPICapture::WASAPICapture() {
    m_hShutdownEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
}

WASAPICapture::~WASAPICapture() {
    StopCapture();
    if (m_hShutdownEvent) {
        CloseHandle(m_hShutdownEvent);
    }
}

// Enumerates active audio output (render) devices.
HRESULT WASAPICapture::EnumerateRenderDevices() {
    m_renderDevices.clear();
    m_renderDeviceNames.clear();

    wil::com_ptr_nothrow<IMMDeviceEnumerator> enumerator;
    RETURN_IF_FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)));

    wil::com_ptr_nothrow<IMMDeviceCollection> collection;
    RETURN_IF_FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection));

    UINT count = 0;
    RETURN_IF_FAILED(collection->GetCount(&count));

    for (UINT i = 0; i < count; i++) {
        wil::com_ptr_nothrow<IMMDevice> device;
        if (SUCCEEDED(collection->Item(i, &device))) {
            wil::com_ptr_nothrow<IPropertyStore> props;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT varName;
                PropVariantInit(&varName);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName))) {
                    m_renderDevices.push_back(device);
                    m_renderDeviceNames.push_back(varName.pwszVal);
                    PropVariantClear(&varName);
                }
            }
        }
    }
    return S_OK;
}

// Enumerates active audio input (capture) devices.
HRESULT WASAPICapture::EnumerateCaptureDevices() {
    m_captureDevices.clear();
    m_captureDeviceNames.clear();

    wil::com_ptr_nothrow<IMMDeviceEnumerator> enumerator;
    RETURN_IF_FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)));

    wil::com_ptr_nothrow<IMMDeviceCollection> collection;
    RETURN_IF_FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection));

    UINT count = 0;
    RETURN_IF_FAILED(collection->GetCount(&count));

    for (UINT i = 0; i < count; i++) {
        wil::com_ptr_nothrow<IMMDevice> device;
        if (SUCCEEDED(collection->Item(i, &device))) {
            wil::com_ptr_nothrow<IPropertyStore> props;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT varName;
                PropVariantInit(&varName);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName))) {
                    m_captureDevices.push_back(device);
                    m_captureDeviceNames.push_back(varName.pwszVal);
                    PropVariantClear(&varName);
                }
            }
        }
    }
    return S_OK;
}

// Starts the capture stream on a selected device.
HRESULT WASAPICapture::StartCapture(int deviceIndex, bool isLoopback) {
    StopCapture();

    wil::com_ptr_nothrow<IMMDevice> device;
    if (isLoopback) {
        if (deviceIndex < 0 || deviceIndex >= m_renderDevices.size()) return E_INVALIDARG;
        device = m_renderDevices[deviceIndex];
    } else {
        if (deviceIndex < 0 || deviceIndex >= m_captureDevices.size()) return E_INVALIDARG;
        device = m_captureDevices[deviceIndex];
    }

    RETURN_IF_FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&m_audioClient));

    WAVEFORMATEX* pwfx = NULL;
    RETURN_IF_FAILED(m_audioClient->GetMixFormat(&pwfx));

    REFERENCE_TIME hnsRequestedDuration = 10000000; // 1 second buffer

    // Set AUDCLNT_STREAMFLAGS_LOOPBACK for capturing speaker output.
    DWORD streamFlags = isLoopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
    streamFlags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

    RETURN_IF_FAILED(m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, hnsRequestedDuration, 0, pwfx, NULL));

    wil::unique_cotaskmem_ptr<WAVEFORMATEX> format(pwfx);

    HANDLE hAudioEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    RETURN_HR_IF_NULL(E_FAIL, hAudioEvent);
    m_audioClient->SetEventHandle(hAudioEvent);

    RETURN_IF_FAILED(m_audioClient->GetService(IID_PPV_ARGS(&m_captureClient)));

    ResetEvent(m_hShutdownEvent);
    m_hCaptureThread = CreateThread(NULL, 0, CaptureThread, this, 0, NULL);
    RETURN_HR_IF_NULL(E_FAIL, m_hCaptureThread);

    m_isCapturing = true;
    RETURN_IF_FAILED(m_audioClient->Start());

    return S_OK;
}

// Stops any active capture stream and cleans up resources.
void WASAPICapture::StopCapture() {
    if (m_isCapturing) {
        if (m_audioClient) m_audioClient->Stop();

        SetEvent(m_hShutdownEvent);
        if (m_hCaptureThread) {
            WaitForSingleObject(m_hCaptureThread, INFINITE);
            CloseHandle(m_hCaptureThread);
            m_hCaptureThread = NULL;
        }

        m_captureClient.reset();
        m_audioClient.reset();
        m_isCapturing = false;
    }
}

// Static entry point for the capture thread.
DWORD WINAPI WASAPICapture::CaptureThread(LPVOID context) {
    WASAPICapture* pThis = static_cast<WASAPICapture*>(context);
    if (SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) {
        pThis->CaptureThreadImpl();
        CoUninitialize();
    }
    return 0;
}

// Main loop for the capture thread.
void WASAPICapture::CaptureThreadImpl() {
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristics(L"Audio", &taskIndex);

    while (WaitForSingleObject(m_hShutdownEvent, 100) == WAIT_TIMEOUT) {
        BYTE* pData;
        UINT32 numFramesAvailable;
        DWORD flags;

        HRESULT hr = m_captureClient->GetBuffer(&pData, &numFramesAvailable, &flags, NULL, NULL);

        if (SUCCEEDED(hr) && numFramesAvailable > 0) {
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                // Audio is silent, no data to process.
            } else {
                // TODO: Process captured audio data from 'pData' buffer.
                // The size of the data is numFramesAvailable * pwfx->nBlockAlign.
            }
            m_captureClient->ReleaseBuffer(numFramesAvailable);
        }
    }

    if (hTask) AvRevertMmThreadCharacteristics(hTask);
}