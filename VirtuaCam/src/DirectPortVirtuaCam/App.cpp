#include "pch.h"
#include "App.h"
#include "UI.h"
#include "WASAPI.h"
#include "Tools.h"
#include "Consumer.h"
#include <wrl.h>
#include <algorithm>

using namespace Microsoft::WRL;

static wil::com_ptr_nothrow<IMFVirtualCamera> g_vcam;
static HWND g_hMainWnd = NULL;
static std::unique_ptr<WASAPICapture> g_audioCapture;
static std::unique_ptr<VirtuaCam::Consumer> g_consumer;

typedef HRESULT (*PFN_InitializeCamera)();
typedef void (*PFN_ShutdownCamera)();
typedef void (*PFN_CycleCameraSource)();
typedef void (*PFN_ToggleMirrorStream)();

static HMODULE g_hCameraDll = nullptr;
static PFN_InitializeCamera g_pfnInitializeCamera = nullptr;
static PFN_ShutdownCamera g_pfnShutdownCamera = nullptr;
static PFN_CycleCameraSource g_pfnCycleCameraSource = nullptr;
static PFN_ToggleMirrorStream g_pfnToggleMirrorStream = nullptr;

static SourceMode g_currentSourceMode = SourceMode::Passthrough;
static bool g_isCameraModuleActive = false;

static DWORD g_preferredPID = 0;

typedef void (*PFN_InitializeBroker)();
typedef void (*PFN_ShutdownBroker)();
typedef void (*PFN_RenderBrokerFrame)();
typedef ID3D11Texture2D* (*PFN_GetSharedTexture)();
typedef BrokerState (*PFN_GetBrokerState)();
typedef void (*PFN_UpdateProducerPriorityList)(const DWORD*, int);
typedef void (*PFN_SetPreferredProducerPID)(DWORD);

static HMODULE g_hBrokerDll = nullptr;
static PFN_InitializeBroker g_pfnInitializeBroker = nullptr;
static PFN_ShutdownBroker g_pfnShutdownBroker = nullptr;
static PFN_RenderBrokerFrame g_pfnRenderBrokerFrame = nullptr;
static PFN_GetSharedTexture g_pfnGetSharedTexture = nullptr;
static PFN_GetBrokerState g_pfnGetBrokerState = nullptr;
static PFN_UpdateProducerPriorityList g_pfnUpdateProducerPriorityList = nullptr;
static PFN_SetPreferredProducerPID g_pfnSetPreferredProducerPID = nullptr;

bool IsRunningAsAdmin();
HRESULT LaunchAndLoadBroker();
HRESULT RegisterVirtualCamera();
void ShutdownSystem();
void OnIdle();
void DiscoverAndInformBroker();

const VirtuaCam::Consumer* GetGlobalConsumer() { return g_consumer.get(); }
DWORD GetPreferredPID() { return g_preferredPID; }
SourceMode GetCurrentSourceMode() { return g_currentSourceMode; }

void SetSourceMode(SourceMode newMode, DWORD discoveredPID = 0) {
    if (newMode == SourceMode::Camera) {
        if (!g_isCameraModuleActive && g_pfnInitializeCamera) {
            g_pfnInitializeCamera();
            g_isCameraModuleActive = true;
        }
        g_preferredPID = GetCurrentProcessId();
    } else {
        if (g_isCameraModuleActive && g_pfnShutdownCamera) {
            g_pfnShutdownCamera();
            g_isCameraModuleActive = false;
        }
        g_preferredPID = (newMode == SourceMode::Discovered) ? discoveredPID : 0;
    }
    
    g_currentSourceMode = newMode;
    if (g_pfnSetPreferredProducerPID) {
        g_pfnSetPreferredProducerPID(g_preferredPID);
    }
    DiscoverAndInformBroker();
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int) {
    if (!IsRunningAsAdmin()) {
        MessageBox(NULL, L"This application requires Administrator privileges to register the virtual camera.", L"Administrator Rights Required", MB_OK | MB_ICONERROR);
        return 1;
    }

    RETURN_IF_FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    RETURN_IF_FAILED(MFStartup(MF_VERSION));

    g_hCameraDll = LoadLibraryW(L"DirectPortCamera.dll");
    if (g_hCameraDll) {
        g_pfnInitializeCamera = (PFN_InitializeCamera)GetProcAddress(g_hCameraDll, "InitializeCamera");
        g_pfnShutdownCamera = (PFN_ShutdownCamera)GetProcAddress(g_hCameraDll, "ShutdownCamera");
        g_pfnCycleCameraSource = (PFN_CycleCameraSource)GetProcAddress(g_hCameraDll, "CycleCameraSource");
        g_pfnToggleMirrorStream = (PFN_ToggleMirrorStream)GetProcAddress(g_hCameraDll, "ToggleMirrorStream");
    }

    if (FAILED(LaunchAndLoadBroker())) {
         MessageBox(NULL, L"Failed to load DirectPortBroker.dll.", L"Error", MB_OK | MB_ICONERROR);
         MFShutdown(); CoUninitialize(); return 1;
    }

    g_consumer = std::make_unique<VirtuaCam::Consumer>();
    ComPtr<ID3D11Device> tempDevice;
    if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, nullptr, 0, D3D11_SDK_VERSION, &tempDevice, nullptr, nullptr))) {
        g_consumer->Initialize(tempDevice.Get());
    }

    UI_Initialize(hInstance, g_hMainWnd, g_pfnGetSharedTexture);
    if (!g_hMainWnd) {
        ShutdownSystem(); MFShutdown(); CoUninitialize(); return FALSE;
    }
    
    SetTimer(g_hMainWnd, 1, 2000, nullptr);
    DiscoverAndInformBroker();

    g_audioCapture = std::make_unique<WASAPICapture>();
    if (SUCCEEDED(g_audioCapture->EnumerateCaptureDevices())) {
        UI_UpdateAudioDeviceLists(g_audioCapture->GetCaptureDeviceNames());
        UI_SetAudioSelectionCallback([](int id) {
            if (id == ID_AUDIO_DEVICE_NONE) g_audioCapture->StopCapture();
            else if (id >= ID_AUDIO_CAPTURE_FIRST) g_audioCapture->StartCapture(id - ID_AUDIO_CAPTURE_FIRST, false);
        });
    }

    if (FAILED(RegisterVirtualCamera())) {
        MessageBox(g_hMainWnd, L"Failed to register and start the virtual camera.", L"Error", MB_OK | MB_ICONERROR);
    }

    UI_RunMessageLoop(OnIdle);

    MFShutdown();
    CoUninitialize();
    return 0;
}

void OnIdle() {
    if (g_pfnRenderBrokerFrame) g_pfnRenderBrokerFrame();
    if (g_pfnGetBrokerState) UpdateTelemetry(g_pfnGetBrokerState());
}

void DiscoverAndInformBroker() {
    if (!g_consumer || !g_pfnUpdateProducerPriorityList) return;
    g_consumer->DiscoverStreams();
    const auto& streams = g_consumer->GetDiscoveredStreams();
    std::vector<DWORD> pids;
    for (const auto& stream : streams) pids.push_back(stream.processId);
    g_pfnUpdateProducerPriorityList(pids.data(), static_cast<int>(pids.size()));
}

HRESULT LaunchAndLoadBroker() {
    g_hBrokerDll = LoadLibraryW(L"DirectPortBroker.dll");
    if (!g_hBrokerDll) return HRESULT_FROM_WIN32(GetLastError());
    g_pfnInitializeBroker = (PFN_InitializeBroker)GetProcAddress(g_hBrokerDll, "InitializeBroker");
    g_pfnShutdownBroker = (PFN_ShutdownBroker)GetProcAddress(g_hBrokerDll, "ShutdownBroker");
    g_pfnRenderBrokerFrame = (PFN_RenderBrokerFrame)GetProcAddress(g_hBrokerDll, "RenderBrokerFrame");
    g_pfnGetSharedTexture = (PFN_GetSharedTexture)GetProcAddress(g_hBrokerDll, "GetSharedTexture");
    g_pfnGetBrokerState = (PFN_GetBrokerState)GetProcAddress(g_hBrokerDll, "GetBrokerState");
    g_pfnUpdateProducerPriorityList = (PFN_UpdateProducerPriorityList)GetProcAddress(g_hBrokerDll, "UpdateProducerPriorityList");
    g_pfnSetPreferredProducerPID = (PFN_SetPreferredProducerPID)GetProcAddress(g_hBrokerDll, "SetPreferredProducerPID");
    if (!g_pfnInitializeBroker || !g_pfnShutdownBroker || !g_pfnRenderBrokerFrame || !g_pfnGetSharedTexture || !g_pfnGetBrokerState || !g_pfnUpdateProducerPriorityList || !g_pfnSetPreferredProducerPID) return E_FAIL;
    g_pfnInitializeBroker();
    return S_OK;
}

HRESULT RegisterVirtualCamera() {
    auto clsid = GUID_ToStringW(CLSID_VCam, false);
    RETURN_IF_FAILED_MSG(MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session, MFVirtualCameraAccess_CurrentUser, L"VirtuaCam", clsid.c_str(), nullptr, 0, &g_vcam), "Failed to create virtual camera");
    RETURN_IF_FAILED_MSG(g_vcam->Start(nullptr), "Cannot start VCam");
    return S_OK;
}

void ShutdownSystem() {
    if (g_audioCapture) g_audioCapture->StopCapture();
    if (g_vcam) { g_vcam->Remove(); g_vcam.reset(); }
    if (g_pfnShutdownBroker) g_pfnShutdownBroker();
    if (g_hBrokerDll) { FreeLibrary(g_hBrokerDll); g_hBrokerDll = nullptr; }
    if (g_pfnShutdownCamera) g_pfnShutdownCamera();
    if (g_hCameraDll) { FreeLibrary(g_hCameraDll); g_hCameraDll = nullptr; }
    UI_Shutdown();
}

bool IsRunningAsAdmin() {
    BOOL fIsAdmin = FALSE; HANDLE hToken = NULL; TOKEN_ELEVATION elevation; DWORD dwSize;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &dwSize)) fIsAdmin = (elevation.TokenIsElevated != 0);
        CloseHandle(hToken);
    }
    return fIsAdmin;
}