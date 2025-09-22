#pragma once
#include <windows.h>
#include <vector>
#include <string>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wil/com.h>

class WASAPICapture {
public:
    WASAPICapture();
    ~WASAPICapture();

    // Enumerate render devices (speakers, headphones) for loopback capture.
    HRESULT EnumerateRenderDevices();
    // Enumerate capture devices (microphones).
    HRESULT EnumerateCaptureDevices();

    // Getters for the device name lists.
    const std::vector<std::wstring>& GetRenderDeviceNames() const { return m_renderDeviceNames; }
    const std::vector<std::wstring>& GetCaptureDeviceNames() const { return m_captureDeviceNames; }

    // Start capturing from a device. isLoopback determines whether to use the render or capture list.
    HRESULT StartCapture(int deviceIndex, bool isLoopback);
    void StopCapture();

private:
    static DWORD WINAPI CaptureThread(LPVOID context);
    void CaptureThreadImpl();

    // Separate lists for render and capture devices.
    std::vector<wil::com_ptr_nothrow<IMMDevice>> m_renderDevices;
    std::vector<std::wstring> m_renderDeviceNames;
    std::vector<wil::com_ptr_nothrow<IMMDevice>> m_captureDevices;
    std::vector<std::wstring> m_captureDeviceNames;
    
    wil::com_ptr_nothrow<IAudioClient> m_audioClient;
    wil::com_ptr_nothrow<IAudioCaptureClient> m_captureClient;

    HANDLE m_hCaptureThread = NULL;
    HANDLE m_hShutdownEvent = NULL;
    bool m_isCapturing = false;
};