#include "pch.h"
#include "Process.h"
#include "Multiplexer.h"
#include <string>
#include <vector>

using namespace Microsoft::WRL;

typedef HRESULT (*PFN_InitializeCameraProducer)(int, const wchar_t*);
typedef HRESULT (*PFN_InitializeCaptureProducer)(HWND, const wchar_t*);
typedef void (*PFN_RunProducer)();
typedef void (*PFN_ShutdownProducer)();

void ShowError(const std::wstring& errorMessage, HRESULT hr = S_OK)
{
    std::wstring finalMessage = errorMessage;
    if (FAILED(hr))
    {
        wchar_t hr_msg[64];
        swprintf_s(hr_msg, L"\nHRESULT: 0x%08X", hr);
        finalMessage += hr_msg;
    }
    MessageBoxW(NULL, finalMessage.c_str(), L"VirtuaCamProcess Error", MB_OK | MB_ICONERROR);
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR lpCmdLine, _In_ int)
{
    RETURN_IF_FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    RETURN_IF_FAILED(MFStartup(MF_VERSION));

    ProcessType type = ProcessType::Unknown;
    int cameraIndex = -1;
    HWND captureHwnd = nullptr;
    DWORD inputPid = 0;

    int argc;
    LPWSTR* argv = CommandLineToArgvW(lpCmdLine, &argc);
    if (argv)
    {
        for (int i = 0; i < argc; ++i)
        {
            if (wcscmp(argv[i], L"/type") == 0 && i + 1 < argc)
            {
                if (_wcsicmp(argv[i + 1], L"camera") == 0) type = ProcessType::Camera;
                else if (_wcsicmp(argv[i + 1], L"capture") == 0) type = ProcessType::Capture;
                else if (_wcsicmp(argv[i + 1], L"multiplexer") == 0) type = ProcessType::Multiplexer;
                i++;
            }
            else if (wcscmp(argv[i], L"/camera_index") == 0 && i + 1 < argc)
            {
                cameraIndex = _wtoi(argv[++i]);
            }
            else if (wcscmp(argv[i], L"/capture_hwnd") == 0 && i + 1 < argc)
            {
                captureHwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(_wtoi64(argv[++i])));
            }
        }
        LocalFree(argv);
    }

    if (type == ProcessType::Unknown)
    {
        ShowError(L"No process type specified. Use /type [camera|capture|multiplexer]");
        return 1;
    }

    if (type == ProcessType::Camera && cameraIndex == -1)
    {
        ShowError(L"No camera index specified for camera type. Use /camera_index [0-N]");
        return 1;
    }

    if (type == ProcessType::Capture && (captureHwnd == nullptr || !IsWindow(captureHwnd)))
    {
        ShowError(L"Invalid or missing HWND for capture type. Use /capture_hwnd [hwnd_value]");
        return 1;
    }

    if (type == ProcessType::Camera)
    {
        HMODULE hDll = LoadLibraryW(L"DirectPortMFCamera.dll");
        if (!hDll) { ShowError(L"Failed to load DirectPortMFCamera.dll", HRESULT_FROM_WIN32(GetLastError())); return 1; }

        auto pfnInitialize = (PFN_InitializeCameraProducer)GetProcAddress(hDll, "InitializeProducer");
        auto pfnRun = (PFN_RunProducer)GetProcAddress(hDll, "RunProducer");
        auto pfnShutdown = (PFN_ShutdownProducer)GetProcAddress(hDll, "ShutdownProducer");

        if (pfnInitialize && pfnRun && pfnShutdown)
        {
            HRESULT hr = pfnInitialize(cameraIndex, L"DirectPort_Producer_Manifest_");
            if (SUCCEEDED(hr))
            {
                pfnRun();
            }
            else
            {
                ShowError(L"Failed to initialize camera producer.", hr);
            }
            pfnShutdown();
        }
        else
        {
            ShowError(L"Could not find required functions in DirectPortMFCamera.dll.", HRESULT_FROM_WIN32(GetLastError()));
        }
        FreeLibrary(hDll);
    }
    else if (type == ProcessType::Capture)
    {
        HMODULE hDll = LoadLibraryW(L"DirectPortMFCapture.dll");
        if (!hDll) { ShowError(L"Failed to load DirectPortMFCapture.dll", HRESULT_FROM_WIN32(GetLastError())); return 1; }

        auto pfnInitialize = (PFN_InitializeCaptureProducer)GetProcAddress(hDll, "InitializeProducer");
        auto pfnRun = (PFN_RunProducer)GetProcAddress(hDll, "RunProducer");
        auto pfnShutdown = (PFN_ShutdownProducer)GetProcAddress(hDll, "ShutdownProducer");

        if (pfnInitialize && pfnRun && pfnShutdown)
        {
            HRESULT hr = pfnInitialize(captureHwnd, L"DirectPort_Producer_Manifest_");
            if (SUCCEEDED(hr))
            {
                pfnRun();
            }
            else
            {
                ShowError(L"Failed to initialize capture producer.", hr);
            }
            pfnShutdown();
        }
        else
        {
            ShowError(L"Could not find required functions in DirectPortMFCapture.dll", HRESULT_FROM_WIN32(GetLastError()));
        }
        FreeLibrary(hDll);
    }
    else if (type == ProcessType::Multiplexer)
    {
        ComPtr<ID3D11Device> device;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr);
        if(FAILED(hr))
        {
            ShowError(L"Failed to create D3D11 device for multiplexer.", hr);
            return 1;
        }

        Multiplexer mux;
        hr = mux.Initialize(device.Get());
        if (SUCCEEDED(hr))
        {
            MSG msg = {};
            while (msg.message != WM_QUIT && msg.message != WM_CLOSE)
            {
                if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
                {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
                else
                {
                    mux.DiscoverAndManageConnections();
                    mux.Composite();
                    Sleep(1);
                }
            }
        }
        else
        {
             ShowError(L"Failed to initialize multiplexer.", hr);
        }
        mux.Shutdown();
    }

    MFShutdown();
    CoUninitialize();
    return 0;
}