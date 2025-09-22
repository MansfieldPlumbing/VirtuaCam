#include "pch.h"
#include "Guids.h"
#include "App.h"
#include "UI.h"
#include "WASAPI.h"
#include "Utilities.h"
#include "Discovery.h"
#include "GraphicsCapture.h"
#include <wrl.h>
#include <memory>
#include <algorithm>
#include <map>
#include <atomic>
#include <mutex>
#include <vector>
#include <functional>
#include <chrono>
#include <thread>
#include <string>
#include <shellapi.h>
#include <d3d11_1.h>

using namespace Microsoft::WRL;

static HWND g_hMainWnd = NULL;
static std::unique_ptr<WASAPICapture> g_audioCapture;
static std::unique_ptr<VirtuaCam::Discovery> g_discovery;
static SourceMode g_currentSourceMode = SourceMode::OFF;
static int g_selectedCameraId = -1;
static HWND g_capturedHwnd = nullptr;
static DWORD g_preferredPID = 0;
static SourceMode g_pipSourceMode = SourceMode::OFF;
static int g_selectedPipCameraId = -1;
static HWND g_capturedPipHwnd = nullptr;
static DWORD g_preferredPipPID = 0;
static std::map<SourceMode, PROCESS_INFORMATION> g_mainWorkerProcesses;
static std::map<SourceMode, PROCESS_INFORMATION> g_pipWorkerProcesses;
static std::recursive_mutex g_stateMutex;
static std::vector<std::function<void()>> g_actionQueue;
static std::mutex g_actionQueueMutex;
typedef void (*PFN_InitializeBroker)(ID3D11Device*);
typedef void (*PFN_ShutdownBroker)();
typedef void (*PFN_RenderBrokerFrame)();
typedef HANDLE (*PFN_GetSharedTextureHandle)();
typedef BrokerState (*PFN_GetBrokerState)();
typedef void (*PFN_UpdateProducerPriorityList)(const DWORD*, int);
typedef void (*PFN_SetPreferredProducerPID)(DWORD);
typedef void (*PFN_SetPipProducerPID)(DWORD);
static HMODULE g_hBrokerDll = nullptr;
static PFN_InitializeBroker g_pfnInitializeBroker = nullptr;
static PFN_ShutdownBroker g_pfnShutdownBroker = nullptr;
static PFN_RenderBrokerFrame g_pfnRenderBrokerFrame = nullptr;
static PFN_GetSharedTextureHandle g_pfnGetSharedTextureHandle = nullptr;
static PFN_GetBrokerState g_pfnGetBrokerState = nullptr;
static PFN_UpdateProducerPriorityList g_pfnUpdateProducerPriorityList = nullptr;
static PFN_SetPreferredProducerPID g_pfnSetPreferredProducerPID = nullptr;
static PFN_SetPipProducerPID g_pfnSetPipProducerPID = nullptr;
static HANDLE g_hUIThread = nullptr;
static DWORD g_dwUIThreadId = 0;
static std::atomic<bool> g_bShutdown = false;
static wil::com_ptr_nothrow<IMFVirtualCamera> g_virtualCamera;
static ComPtr<ID3D11Device> g_d3d11Device;
HRESULT LaunchAndLoadBroker(ID3D11Device* pDevice);
void ShutdownSystem();
void OnIdle();
void DiscoverAndInformBroker();
void ProcessActionQueue();
void ShutdownWorker(PROCESS_INFORMATION& pi);
HRESULT LaunchWorker(const std::wstring& args, PROCESS_INFORMATION& pi);
void SendIpcCommand(VirtuaCamCommand command);

const VirtuaCam::Discovery* GetGlobalDiscovery() { return g_discovery.get(); }
DWORD GetPreferredPID() { std::lock_guard<std::recursive_mutex> lock(g_stateMutex); return g_preferredPID; }
DWORD GetPreferredPipPID() { std::lock_guard<std::recursive_mutex> lock(g_stateMutex); return g_preferredPipPID; }
SourceMode GetCurrentSourceMode() { std::lock_guard<std::recursive_mutex> lock(g_stateMutex); return g_currentSourceMode; }
SourceMode GetPipSourceMode() { std::lock_guard<std::recursive_mutex> lock(g_stateMutex); return g_pipSourceMode; }
int GetSelectedCameraId() { std::lock_guard<std::recursive_mutex> lock(g_stateMutex); return g_selectedCameraId; }
int GetSelectedPipCameraId() { std::lock_guard<std::recursive_mutex> lock(g_stateMutex); return g_selectedPipCameraId; }
HWND GetCapturedHwnd() { std::lock_guard<std::recursive_mutex> lock(g_stateMutex); return g_capturedHwnd; }
HWND GetCapturedPipHwnd() { std::lock_guard<std::recursive_mutex> lock(g_stateMutex); return g_capturedPipHwnd; }

void SetSourceMode(SourceMode newMode, DWORD_PTR context = 0) {
    std::lock_guard<std::mutex> lock(g_actionQueueMutex);
    g_actionQueue.emplace_back([=]() {
        std::lock_guard<std::recursive_mutex> stateLock(g_stateMutex);
        if (g_pfnSetPreferredProducerPID) {
            g_pfnSetPreferredProducerPID(0);
        }
        for (auto& pair : g_mainWorkerProcesses) {
            ShutdownWorker(pair.second);
        }
        g_mainWorkerProcesses.clear();
        g_preferredPID = 0;
        g_selectedCameraId = -1;
        g_capturedHwnd = nullptr;
        g_currentSourceMode = SourceMode::OFF;
        DiscoverAndInformBroker();
        g_currentSourceMode = newMode;
        std::wstring workerArgs;
        PROCESS_INFORMATION pi = {};
        switch (newMode) {
            case SourceMode::HardwareCamera:
                g_selectedCameraId = static_cast<int>(context);
                workerArgs = L"/type camera /camera_index " + std::to_wstring(g_selectedCameraId);
                break;
            case SourceMode::Window:
                g_capturedHwnd = reinterpret_cast<HWND>(context);
                workerArgs = L"/type capture /capture_hwnd " + std::to_wstring(reinterpret_cast<uintptr_t>(g_capturedHwnd));
                break;
            case SourceMode::Multiplexer:
                workerArgs = L"/type multiplexer";
                break;
            case SourceMode::Discovered:
                g_preferredPID = static_cast<DWORD>(context);
                break;
            case SourceMode::Passthrough:
            case SourceMode::OFF:
            default:
                break;
        }
        if (!workerArgs.empty())
        {
            if (SUCCEEDED(LaunchWorker(workerArgs, pi))) {
                g_mainWorkerProcesses[newMode] = pi;
                g_preferredPID = pi.dwProcessId;
            } else {
                MessageBoxW(g_hMainWnd, (L"Failed to launch worker process for mode: " + workerArgs).c_str(), L"Worker Launch Error", MB_OK | MB_ICONERROR);
                g_currentSourceMode = SourceMode::OFF;
            }
        }
        if (g_pfnSetPreferredProducerPID) {
            g_pfnSetPreferredProducerPID(g_preferredPID);
        }
        DiscoverAndInformBroker();
    });
}

void SetPipMode(SourceMode newMode, DWORD_PTR context) {
    std::lock_guard<std::mutex> lock(g_actionQueueMutex);
    g_actionQueue.emplace_back([=]() {
        std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
        if (g_pfnSetPipProducerPID) {
            g_pfnSetPipProducerPID(0);
        }
        for (auto& pair : g_pipWorkerProcesses) {
            ShutdownWorker(pair.second);
        }
        g_pipWorkerProcesses.clear();
        g_preferredPipPID = 0;
        g_selectedPipCameraId = -1;
        g_capturedPipHwnd = nullptr;
        g_pipSourceMode = newMode;
        std::wstring workerArgs;
        PROCESS_INFORMATION pi = {};
        switch (newMode) {
            case SourceMode::HardwareCamera:
                g_selectedPipCameraId = static_cast<int>(context);
                workerArgs = L"/type camera /camera_index " + std::to_wstring(g_selectedPipCameraId);
                break;
            case SourceMode::Window:
                g_capturedPipHwnd = reinterpret_cast<HWND>(context);
                workerArgs = L"/type capture /capture_hwnd " + std::to_wstring(reinterpret_cast<uintptr_t>(g_capturedPipHwnd));
                break;
            case SourceMode::Multiplexer:
                workerArgs = L"/type multiplexer";
                break;
            case SourceMode::Discovered:
                g_preferredPipPID = static_cast<DWORD>(context);
                break;
            case SourceMode::Passthrough:
            case SourceMode::OFF:
            default:
                break;
        }
        if (!workerArgs.empty())
        {
            if (SUCCEEDED(LaunchWorker(workerArgs, pi))) {
                g_pipWorkerProcesses[newMode] = pi;
                g_preferredPipPID = pi.dwProcessId;
            } else {
                MessageBoxW(g_hMainWnd, (L"Failed to launch PIP worker process: " + workerArgs).c_str(), L"PIP Worker Launch Error", MB_OK | MB_ICONERROR);
                g_pipSourceMode = SourceMode::OFF;
            }
        }
        if (g_pfnSetPipProducerPID) {
            g_pfnSetPipProducerPID(g_preferredPipPID);
        }
    });
}

struct UIThreadParams {
    HINSTANCE hInstance;
    ID3D11Device* pDevice;
};

DWORD WINAPI UIThreadProc(LPVOID lpParameter)
{
    UIThreadParams* params = static_cast<UIThreadParams*>(lpParameter);
    THROW_IF_FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    auto coUninit = wil::scope_exit([&] { CoUninitialize(); });

    UI_Initialize(params->hInstance, params->pDevice, g_hMainWnd, g_pfnGetSharedTextureHandle);
    delete params;
    THROW_HR_IF(E_FAIL, !g_hMainWnd);
    
    SetTimer(g_hMainWnd, 1, 2000, nullptr);
    DiscoverAndInformBroker();
    g_audioCapture = std::make_unique<WASAPICapture>();
    if (SUCCEEDED(g_audioCapture->EnumerateCaptureDevices()))
    {
        UI_UpdateAudioDeviceLists(g_audioCapture->GetCaptureDeviceNames());
        UI_SetAudioSelectionCallback([](int id)
                                     {
            if (id == ID_AUDIO_DEVICE_NONE) g_audioCapture->StopCapture();
            else if (id >= ID_AUDIO_CAPTURE_FIRST) g_audioCapture->StartCapture(id - ID_AUDIO_CAPTURE_FIRST, false); });
    }
    UI_RunMessageLoop([]() {});
    
    return 0;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int) {
    try
    {
        THROW_IF_FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        auto coUninit = wil::scope_exit([&] { CoUninitialize(); });

        THROW_IF_FAILED(MFStartup(MF_VERSION));
        auto mfShutdown = wil::scope_exit([&] { MFShutdown(); });
        
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        UINT d3dFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
        d3dFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        THROW_IF_FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, NULL, d3dFlags, nullptr, 0, D3D11_SDK_VERSION, &g_d3d11Device, nullptr, nullptr));
        
        ComPtr<ID3D11Multithread> d3dMultiThread;
        if (SUCCEEDED(g_d3d11Device.As(&d3dMultiThread)))
        {
            d3dMultiThread->SetMultithreadProtected(TRUE);
        }

        THROW_IF_FAILED(LaunchAndLoadBroker(g_d3d11Device.Get()));
        auto brokerShutdown = wil::scope_exit([&] {
            if (g_pfnShutdownBroker) g_pfnShutdownBroker();
            if (g_hBrokerDll) { FreeLibrary(g_hBrokerDll); g_hBrokerDll = nullptr; }
        });

        auto clsidString = VirtuaCam::Utils::Debug::GuidToWString(CLSID_VirtuaCam, false);
        THROW_IF_FAILED(MFCreateVirtualCamera(
            MFVirtualCameraType_SoftwareCameraSource,
            MFVirtualCameraLifetime_Session,
            MFVirtualCameraAccess_CurrentUser,
            L"VirtuaCam",
            clsidString.c_str(),
            nullptr,
            0,
            &g_virtualCamera
        ));
        
        auto vcamRemove = wil::scope_exit([&] { 
            if(g_virtualCamera) {
                g_virtualCamera->Shutdown(); 
                g_virtualCamera->Remove();
            }
        });
        
        THROW_IF_FAILED(g_virtualCamera->Start(nullptr));
        
        g_discovery = std::make_unique<VirtuaCam::Discovery>();
        g_discovery->Initialize(g_d3d11Device.Get());

        UIThreadParams* uiParams = new UIThreadParams{ hInstance, g_d3d11Device.Get() };
        g_hUIThread = CreateThread(nullptr, 0, UIThreadProc, uiParams, 0, &g_dwUIThreadId);
        THROW_HR_IF_NULL(E_FAIL, g_hUIThread);
        auto threadCloser = wil::scope_exit([&] { if (g_hUIThread) { CloseHandle(g_hUIThread); } });

        while (g_hMainWnd == NULL)
        {
            Sleep(10);
        }

        while (!g_bShutdown)
        {
            OnIdle();
            Sleep(1);
        }

        if (g_hUIThread)
        {
            WaitForSingleObject(g_hUIThread, INFINITE);
        }
    }
    catch (const wil::ResultException& e)
    {
        wchar_t message[2048];
        wchar_t error[1024];
        if (SUCCEEDED(wil::GetFailureLogString(error, _countof(error), e.GetFailureInfo())))
        {
            StringCchPrintfW(message, _countof(message), L"A fatal error occurred:\n\n%s", error);
        }
        else
        {
            StringCchPrintfW(message, _countof(message), L"A fatal error occurred:\n\nHRESULT: 0x%08X", e.GetErrorCode());
        }
        MessageBoxW(nullptr, message, L"VirtuaCam Runtime Error", MB_OK | MB_ICONERROR);
    }
    catch (const winrt::hresult_error& e)
    {
        wchar_t message[1024];
        StringCchPrintfW(message, _countof(message), L"A fatal WinRT error occurred:\n\n%s\nHRESULT: 0x%08X", e.message().c_str(), e.code());
        MessageBoxW(nullptr, message, L"VirtuaCam Runtime Error", MB_OK | MB_ICONERROR);
    }
    catch (...)
    {
        MessageBoxW(nullptr, L"An unknown fatal error occurred.", L"VirtuaCam Runtime Error", MB_OK | MB_ICONERROR);
    }
    
    for (auto& pair : g_mainWorkerProcesses) ShutdownWorker(pair.second);
    for (auto& pair : g_pipWorkerProcesses) ShutdownWorker(pair.second);
    if (g_audioCapture) g_audioCapture->StopCapture();

    return 0;
}

void ProcessActionQueue() {
    std::vector<std::function<void()>> actions;
    {
        std::lock_guard<std::mutex> lock(g_actionQueueMutex);
        if (!g_actionQueue.empty()) {
            actions.swap(g_actionQueue);
        }
    }
    for (const auto& action : actions) {
        action();
    }
}

void OnIdle() {
    if (g_pfnRenderBrokerFrame) {
        g_pfnRenderBrokerFrame();
    }
    
    ProcessActionQueue();
    
    if (g_pfnGetBrokerState) {
        UpdateTelemetry(g_pfnGetBrokerState());
    }
}

void DiscoverAndInformBroker() {
    std::lock_guard<std::recursive_mutex> lock(g_stateMutex);
    if (!g_discovery || !g_pfnUpdateProducerPriorityList) return;
    g_discovery->DiscoverStreams();
    const auto& streams = g_discovery->GetDiscoveredStreams();
    std::vector<DWORD> pids;
    for (const auto& stream : streams) {
        pids.push_back(stream.processId);
    }
    g_pfnUpdateProducerPriorityList(pids.data(), static_cast<int>(pids.size()));
}

HRESULT LaunchAndLoadBroker(ID3D11Device* pDevice) {
    g_hBrokerDll = LoadLibraryW(L"DirectPortBroker.dll");
    if (!g_hBrokerDll) return HRESULT_FROM_WIN32(GetLastError());
    g_pfnInitializeBroker = (PFN_InitializeBroker)GetProcAddress(g_hBrokerDll, "InitializeBroker");
    g_pfnShutdownBroker = (PFN_ShutdownBroker)GetProcAddress(g_hBrokerDll, "ShutdownBroker");
    g_pfnRenderBrokerFrame = (PFN_RenderBrokerFrame)GetProcAddress(g_hBrokerDll, "RenderBrokerFrame");
    g_pfnGetSharedTextureHandle = (PFN_GetSharedTextureHandle)GetProcAddress(g_hBrokerDll, "GetSharedTextureHandle");
    g_pfnGetBrokerState = (PFN_GetBrokerState)GetProcAddress(g_hBrokerDll, "GetBrokerState");
    g_pfnUpdateProducerPriorityList = (PFN_UpdateProducerPriorityList)GetProcAddress(g_hBrokerDll, "UpdateProducerPriorityList");
    g_pfnSetPreferredProducerPID = (PFN_SetPreferredProducerPID)GetProcAddress(g_hBrokerDll, "SetPreferredProducerPID");
    g_pfnSetPipProducerPID = (PFN_SetPipProducerPID)GetProcAddress(g_hBrokerDll, "SetPipProducerPID");
    if (!g_pfnInitializeBroker || !g_pfnShutdownBroker || !g_pfnRenderBrokerFrame || !g_pfnGetSharedTextureHandle || !g_pfnGetBrokerState || !g_pfnUpdateProducerPriorityList || !g_pfnSetPreferredProducerPID || !g_pfnSetPipProducerPID) return E_FAIL;
    g_pfnInitializeBroker(pDevice);
    return S_OK;
}

void ShutdownSystem() {
    if (g_bShutdown) return;
    g_bShutdown = true;
    if (g_dwUIThreadId != 0) {
        PostThreadMessage(g_dwUIThreadId, WM_QUIT, 0, 0);
    }
    UI_Shutdown();
}

void ShutdownWorker(PROCESS_INFORMATION& pi)
{
    if (pi.hProcess)
    {
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        memset(&pi, 0, sizeof(PROCESS_INFORMATION));
    }
}

HRESULT LaunchWorker(const std::wstring& args, PROCESS_INFORMATION& pi)
{
    ShutdownWorker(pi);
    WCHAR szExePath[MAX_PATH];
    GetModuleFileNameW(NULL, szExePath, MAX_PATH);
    std::wstring exeDir = szExePath;
    exeDir = exeDir.substr(0, exeDir.find_last_of(L"\\/"));
    std::wstring workerPath = exeDir + L"\\VirtuaCamProcess.exe";
    WCHAR szCmdLine[1024];
    StringCchPrintfW(szCmdLine, _countof(szCmdLine), L"\"%s\" %s", workerPath.c_str(), args.c_str());
    STARTUPINFOW si = { sizeof(si) };
    if (CreateProcessW(NULL, szCmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        std::wstring manifestName = L"DirectPort_Producer_Manifest_" + std::to_wstring(pi.dwProcessId);
        HANDLE hManifest = NULL;
        for (int i = 0; i < 200; ++i) 
        {
            hManifest = OpenFileMappingW(FILE_MAP_READ, FALSE, manifestName.c_str());
            if (hManifest)
            {
                CloseHandle(hManifest);
                return S_OK;
            }
            Sleep(10);
        }
        
        ShutdownWorker(pi);
        return E_FAIL;
    }
    return HRESULT_FROM_WIN32(GetLastError());
}

void SendIpcCommand(VirtuaCamCommand command)
{
    if (!g_virtualCamera)
    {
        return;
    }

    std::wstring pipeNameStr = L"\\\\.\\pipe\\VirtuaCam_IPC_" + VirtuaCam::Utils::Debug::GuidToWString(CLSID_VirtuaCam, false);
    
    HANDLE hPipe = CreateFileW(
        pipeNameStr.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (hPipe != INVALID_HANDLE_VALUE)
    {
        DWORD bytesWritten;
        WriteFile(hPipe, &command, sizeof(DWORD), &bytesWritten, nullptr);
        CloseHandle(hPipe);
    }
}