// --- src/DirectPortVirtuaCam/GraphicsCapture.h ---
#pragma once
#include <vector>
#include <string>
#include <memory>
#include <windows.h>
#include <d3d11.h>

#ifdef MFCAPTURE_EXPORTS
#define MFCAPTURE_API __declspec(dllexport)
#else
#define MFCAPTURE_API __declspec(dllimport)
#endif

struct WindowInfo {
    HWND hwnd;
    std::wstring title;
};

class GraphicsCapture
{
public:
    GraphicsCapture();
    ~GraphicsCapture();

    HRESULT Initialize(ID3D11Device* device);
    void Shutdown();

    static std::vector<WindowInfo> EnumerateWindows();
    HRESULT StartCapture(HWND hwnd, const std::wstring& manifestPrefix = L"DirectPort_Producer_Manifest_");
    void StopCapture();

    bool IsActive() const;
    HWND GetCapturedHwnd() const;

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

#ifdef __cplusplus
extern "C" {
#endif

MFCAPTURE_API HRESULT InitializeProducer(HWND hwnd, const wchar_t* manifestPrefix);
MFCAPTURE_API void RunProducer();
MFCAPTURE_API void ShutdownProducer();

#ifdef __cplusplus
}
#endif
