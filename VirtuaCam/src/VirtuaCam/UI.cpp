#include "pch.h"
#include "App.h"
#include "UI.h"
#include "Menu.h"
#include "Formats.h"
#include "Discovery.h"
#include "GraphicsCapture.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <dwmapi.h>
#include <mfreadwrite.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "mfreadwrite.lib")

using namespace Microsoft::WRL;

namespace VirtuaCam { class Discovery; }
extern const VirtuaCam::Discovery* GetGlobalDiscovery();
extern DWORD GetPreferredPID();
extern void SetPreferredPID(DWORD pid);
extern void DiscoverAndInformBroker();
void ShutdownSystem();

extern SourceMode GetCurrentSourceMode();
extern void SetSourceMode(SourceMode newMode, DWORD_PTR context);
extern SourceMode GetPipSourceMode();
extern void SetPipMode(SourceMode newMode, DWORD_PTR context);
extern int GetSelectedPipCameraId();
extern HWND GetCapturedPipHwnd();
extern DWORD GetPreferredPipPID();
extern int GetSelectedCameraId();
extern HWND GetCapturedHwnd();


static HINSTANCE g_instance;
static HWND g_hMainWnd = NULL;
static HWND g_hPreviewWnd = NULL;
static HWND g_hTelemetryLabel = NULL;
static WCHAR g_windowClass[MAX_LOADSTRING];
static std::function<void(int)> g_audioSelectionCallback;
static std::vector<std::wstring> g_captureDeviceNames;
static std::vector<std::wstring> g_renderDeviceNames;
static int g_currentAudioDevice = ID_AUDIO_DEVICE_NONE;
static PFN_GetSharedTextureHandle g_pfnGetSharedTextureHandle = nullptr;
static std::function<void()> g_onIdle;

static ComPtr<ID3D11Device> g_device;
static ComPtr<ID3D11Device1> g_device1;
static ComPtr<ID3D11DeviceContext> g_context;
static ComPtr<IDXGISwapChain> g_swapChain;
static ComPtr<ID3D11RenderTargetView> g_rtv;
static ComPtr<ID3D11VertexShader> g_vs;
static ComPtr<ID3D11PixelShader> g_ps;
static ComPtr<ID3D11SamplerState> g_sampler;

static std::map<UINT, HWND> g_windowMenuMap;
static std::map<UINT, HWND> g_pipWindowMenuMap;

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK PreviewWndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);
void AddTrayIcon(HWND hwnd, bool add);
void ShowContextMenu(HWND hwnd);
ATOM MyRegisterClass(HINSTANCE instance);
HRESULT InitD3D(HWND hwnd);
void CleanupD3D();
void RenderPreviewFrame(HWND hwnd);
HRESULT LoadAssets();
std::vector<std::wstring> EnumeratePhysicalCameras();

const char* g_vertexShaderHLSL = R"(
struct VOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
VOut main(uint vid : SV_VertexID) {
    float2 uv = float2((vid << 1) & 2, vid & 2);
    VOut o; o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1);
    o.uv = uv; return o;
})";

const char* g_pixelShaderHLSL = R"(
Texture2D    tex : register(t0); SamplerState smp : register(s0);
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_Target {
    return tex.Sample(smp, uv);
})";

void UI_Initialize(HINSTANCE instance, ID3D11Device* pDevice, HWND& outMainWnd, PFN_GetSharedTextureHandle pfnGetSharedTextureHandle) {
    g_instance = instance;
    g_device = pDevice;
    g_device.As(&g_device1);
    g_device->GetImmediateContext(&g_context);
    g_pfnGetSharedTextureHandle = pfnGetSharedTextureHandle;
    LoadStringW(instance, IDC_VIRTUACAM, g_windowClass, MAX_LOADSTRING);
    MyRegisterClass(instance);

    g_hMainWnd = CreateWindowW(g_windowClass, L"VirtuaCam", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, 200, 100, nullptr, nullptr, instance, nullptr);

    if (g_hMainWnd) {
        AddTrayIcon(g_hMainWnd, true);
    }
    outMainWnd = g_hMainWnd;
}

void UI_RunMessageLoop(std::function<void()> onIdle) {
    g_onIdle = onIdle;
    MSG msg = {};
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else
        {
            if (g_onIdle)
            {
                g_onIdle();
            }

            if (g_hPreviewWnd && IsWindow(g_hPreviewWnd))
            {
                RenderPreviewFrame(g_hPreviewWnd);
            }
            else
            {
                Sleep(1);
            }
        }
    }
}


void UI_Shutdown() {
    AddTrayIcon(g_hMainWnd, false);
    CleanupD3D();
}

void UI_UpdateAudioDeviceLists(const std::vector<std::wstring>& captureDevices) {
    g_captureDeviceNames = captureDevices;
    g_renderDeviceNames.clear();
}

void UI_SetAudioSelectionCallback(std::function<void(int)> callback) {
    g_audioSelectionCallback = callback;
}

ATOM MyRegisterClass(HINSTANCE instance) {
    WNDCLASSEXW wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = instance;
    wcex.lpszClassName = g_windowClass;
    wcex.hIcon = LoadIcon(instance, MAKEINTRESOURCE(IDI_VIRTUACAM));
    wcex.hIconSm = LoadIcon(instance, MAKEINTRESOURCE(IDI_SMALL));
    RegisterClassExW(&wcex);

    WNDCLASSEXW wcexPreview = {};
    wcexPreview.cbSize = sizeof(WNDCLASSEX);
    wcexPreview.style = CS_HREDRAW | CS_VREDRAW;
    wcexPreview.lpfnWndProc = PreviewWndProc;
    wcexPreview.hInstance = instance;
    wcexPreview.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcexPreview.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcexPreview.lpszClassName = PREVIEW_WINDOW_CLASS;
    wcexPreview.hIcon = LoadIcon(instance, MAKEINTRESOURCE(IDI_VIRTUACAM));
    wcexPreview.hIconSm = LoadIcon(instance, MAKEINTRESOURCE(IDI_SMALL));
    return RegisterClassExW(&wcexPreview);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_TIMER:
        if (wParam == 1) {
            DiscoverAndInformBroker();
        }
        break;
    case WM_APP_TRAY_MSG:
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
            ShowContextMenu(hwnd);
        } else if (lParam == WM_LBUTTONDBLCLK) {
            CreatePreviewWindow();
        }
        break;
    case WM_APP_MENU_COMMAND:
        {
            UINT result = static_cast<UINT>(wParam);
            if (result >= ID_SOURCE_OFF && result < ID_PIP_OFF)
            {
                if (result == ID_SOURCE_OFF) {
                    SetSourceMode(SourceMode::OFF, 0);
                } else if (result == ID_SOURCE_PASSTHROUGH) {
                    SetSourceMode(SourceMode::Passthrough, 0);
                } else if (result >= ID_SOURCE_HW_CAMERA_FIRST && result < ID_SOURCE_WINDOW_FIRST) {
                    int cameraId = result - ID_SOURCE_HW_CAMERA_FIRST;
                    SetSourceMode(SourceMode::HardwareCamera, cameraId);
                } else if (result >= ID_SOURCE_WINDOW_FIRST) {
                    if(g_windowMenuMap.count(result)) {
                        HWND hwndToCapture = g_windowMenuMap[result];
                        SetSourceMode(SourceMode::Window, reinterpret_cast<DWORD_PTR>(hwndToCapture));
                    }
                } else if (result >= ID_SOURCE_DISCOVERED_FIRST && result < ID_SOURCE_HW_CAMERA_FIRST) {
                    const VirtuaCam::Discovery* discovery = GetGlobalDiscovery();
                    if (discovery) {
                        const auto& streams = discovery->GetDiscoveredStreams();
                        std::vector<VirtuaCam::DiscoveredSharedStream> filteredStreams;
                        for (const auto& stream : streams) {
                            if (stream.processName != L"VirtuaCam.exe" && stream.processId != GetCurrentProcessId()) {
                                filteredStreams.push_back(stream);
                            }
                        }
                        int index = result - ID_SOURCE_DISCOVERED_FIRST;
                        if (index >= 0 && (size_t)index < filteredStreams.size()) {
                            SetSourceMode(SourceMode::Discovered, filteredStreams[index].processId);
                        }
                    }
                }
            }
            else if (result >= ID_PIP_OFF)
            {
                if (result == ID_PIP_OFF) {
                    SetPipMode(SourceMode::OFF, 0);
                } else if (result >= ID_PIP_HW_CAMERA_FIRST && result < ID_PIP_WINDOW_FIRST) {
                    int cameraId = result - ID_PIP_HW_CAMERA_FIRST;
                    SetPipMode(SourceMode::HardwareCamera, cameraId);
                } else if (result >= ID_PIP_WINDOW_FIRST) {
                    if(g_pipWindowMenuMap.count(result)) {
                        HWND hwndToCapture = g_pipWindowMenuMap[result];
                        SetPipMode(SourceMode::Window, reinterpret_cast<DWORD_PTR>(hwndToCapture));
                    }
                } else if (result >= ID_PIP_DISCOVERED_FIRST && result < ID_PIP_HW_CAMERA_FIRST) {
                    const VirtuaCam::Discovery* discovery = GetGlobalDiscovery();
                    if (discovery) {
                        const auto& streams = discovery->GetDiscoveredStreams();
                        std::vector<VirtuaCam::DiscoveredSharedStream> filteredStreams;
                        for (const auto& stream : streams) {
                            if (stream.processName != L"VirtuaCam.exe" && stream.processId != GetCurrentProcessId()) {
                                filteredStreams.push_back(stream);
                            }
                        }
                        int index = result - ID_PIP_DISCOVERED_FIRST;
                        if (index >= 0 && (size_t)index < filteredStreams.size()) {
                            SetPipMode(SourceMode::Discovered, filteredStreams[index].processId);
                        }
                    }
                }
            }
            else if (result >= ID_AUDIO_DEVICE_NONE && result < ID_SOURCE_OFF) {
                g_currentAudioDevice = result;
                if (g_audioSelectionCallback) g_audioSelectionCallback(result);
            } else if (result == ID_TRAY_PREVIEW_WINDOW) {
                CreatePreviewWindow();
            } else if (result == ID_TRAY_ABOUT) {
                DialogBox(g_instance, MAKEINTRESOURCE(IDD_ABOUTBOX), hwnd, About);
            } else if (result == ID_TRAY_EXIT) {
                DestroyWindow(hwnd);
            }
        }
        break;
    case WM_DESTROY:
        ShutdownSystem();
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, message, wParam, lParam);
    }
    return 0;
}

std::vector<std::wstring> EnumeratePhysicalCameras() {
    std::vector<std::wstring> cameraNames;
    wil::com_ptr_nothrow<IMFAttributes> pAttributes;
    if (FAILED(MFCreateAttributes(&pAttributes, 1))) return cameraNames;
    if (FAILED(pAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) return cameraNames;

    IMFActivate** ppDevices = nullptr;
    UINT32 count = 0;
    if (SUCCEEDED(MFEnumDeviceSources(pAttributes.get(), &ppDevices, &count))) {
        for (UINT32 i = 0; i < count; i++) {
            WCHAR* friendlyName = nullptr;
            UINT32 friendlyNameLength = 0;
            if (SUCCEEDED(ppDevices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &friendlyName, &friendlyNameLength))) {
                if (std::wstring(friendlyName).find(L"VirtuaCam") == std::wstring::npos) {
                    cameraNames.push_back(friendlyName);
                }
                CoTaskMemFree(friendlyName);
            }
            ppDevices[i]->Release();
        }
        CoTaskMemFree(ppDevices);
    }
    return cameraNames;
}

void ShowContextMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    g_windowMenuMap.clear();
    g_pipWindowMenuMap.clear();

    CustomMenu* menu = new CustomMenu(hwnd, g_instance);
    menu->AddItem(L"Show Preview", ID_TRAY_PREVIEW_WINDOW);
    menu->AddSeparator();
    
    CustomMenu* sourcesSubMenu = menu->AddSubMenu(L"Source");
    if (sourcesSubMenu) {
        SourceMode currentMode = GetCurrentSourceMode();
        DWORD preferredPID = GetPreferredPID();
        int selectedCameraId = GetSelectedCameraId();
        HWND capturedHwnd = GetCapturedHwnd();

        sourcesSubMenu->AddItem(L"OFF", ID_SOURCE_OFF, currentMode == SourceMode::OFF);
        sourcesSubMenu->AddSeparator();
        sourcesSubMenu->AddItem(L"Passthrough", ID_SOURCE_PASSTHROUGH, currentMode == SourceMode::Passthrough);

        auto cameras = EnumeratePhysicalCameras();
        if (!cameras.empty()) {
            sourcesSubMenu->AddSeparator();
            for (size_t i = 0; i < cameras.size(); ++i) {
                std::wstring cameraName = cameras[i];
                if (cameraName.length() > 32) {
                    cameraName = cameraName.substr(0, 32) + L"...";
                }
                sourcesSubMenu->AddItem(cameraName, ID_SOURCE_HW_CAMERA_FIRST + (UINT)i, currentMode == SourceMode::HardwareCamera && selectedCameraId == (int)i);
            }
        }

        auto windows = GraphicsCapture::EnumerateWindows();
        if (!windows.empty()) {
            sourcesSubMenu->AddSeparator();
            for (size_t i = 0; i < windows.size(); ++i) {
                UINT menuId = ID_SOURCE_WINDOW_FIRST + (UINT)i;
                g_windowMenuMap[menuId] = windows[i].hwnd;
                std::wstring windowTitle = windows[i].title;
                if (windowTitle.length() > 32) {
                    windowTitle = windowTitle.substr(0, 32) + L"...";
                }
                sourcesSubMenu->AddItem(windowTitle, menuId, currentMode == SourceMode::Window && capturedHwnd == windows[i].hwnd);
            }
        }
        
        const VirtuaCam::Discovery* discovery = GetGlobalDiscovery();
        if (discovery) {
            const auto& streams = discovery->GetDiscoveredStreams();
            std::vector<VirtuaCam::DiscoveredSharedStream> filteredStreams;
            for (const auto& stream : streams) {
                if (stream.processName != L"VirtuaCam.exe" && stream.processId != GetCurrentProcessId()) {
                    filteredStreams.push_back(stream);
                }
            }

            if (!filteredStreams.empty()) {
                sourcesSubMenu->AddSeparator();
                for (size_t i = 0; i < filteredStreams.size(); ++i) {
                    const auto& stream = filteredStreams[i];
                    std::wstring label = stream.producerType + L": " + stream.processName + L" (PID: " + std::to_wstring(stream.processId) + L")";
                    if (label.length() > 32) {
                        label = label.substr(0, 32) + L"...";
                    }
                    sourcesSubMenu->AddItem(label, ID_SOURCE_DISCOVERED_FIRST + (UINT)i, currentMode == SourceMode::Discovered && preferredPID == stream.processId);
                }
            }
        }
    }
    
    CustomMenu* pipSubMenu = menu->AddSubMenu(L"PIP");
    if (pipSubMenu) {
        SourceMode pipMode = GetPipSourceMode();
        int pipCamId = GetSelectedPipCameraId();
        HWND pipHwnd = GetCapturedPipHwnd();
        DWORD pipPID = GetPreferredPipPID();

        pipSubMenu->AddItem(L"OFF", ID_PIP_OFF, pipMode == SourceMode::OFF);
        pipSubMenu->AddSeparator();

        auto cameras = EnumeratePhysicalCameras();
        if (!cameras.empty()) {
            for (size_t i = 0; i < cameras.size(); ++i) {
                std::wstring cameraName = cameras[i];
                if (cameraName.length() > 32) {
                    cameraName = cameraName.substr(0, 32) + L"...";
                }
                pipSubMenu->AddItem(cameraName, ID_PIP_HW_CAMERA_FIRST + (UINT)i, pipMode == SourceMode::HardwareCamera && pipCamId == (int)i);
            }
        }

        auto windows = GraphicsCapture::EnumerateWindows();
        if (!windows.empty()) {
            pipSubMenu->AddSeparator();
            for (size_t i = 0; i < windows.size(); ++i) {
                UINT menuId = ID_PIP_WINDOW_FIRST + (UINT)i;
                g_pipWindowMenuMap[menuId] = windows[i].hwnd;
                std::wstring windowTitle = windows[i].title;
                if (windowTitle.length() > 32) {
                    windowTitle = windowTitle.substr(0, 32) + L"...";
                }
                pipSubMenu->AddItem(windowTitle, menuId, pipMode == SourceMode::Window && pipHwnd == windows[i].hwnd);
            }
        }
        
        const VirtuaCam::Discovery* discovery = GetGlobalDiscovery();
        if (discovery) {
            const auto& streams = discovery->GetDiscoveredStreams();
            std::vector<VirtuaCam::DiscoveredSharedStream> filteredStreams;
            for (const auto& stream : streams) {
                if (stream.processName != L"VirtuaCam.exe" && stream.processId != GetCurrentProcessId()) {
                    filteredStreams.push_back(stream);
                }
            }

            if (!filteredStreams.empty()) {
                pipSubMenu->AddSeparator();
                for (size_t i = 0; i < filteredStreams.size(); ++i) {
                    const auto& stream = filteredStreams[i];
                    std::wstring label = stream.producerType + L": " + stream.processName + L" (PID: " + std::to_wstring(stream.processId) + L")";
                    if (label.length() > 32) {
                        label = label.substr(0, 32) + L"...";
                    }
                    pipSubMenu->AddItem(label, ID_PIP_DISCOVERED_FIRST + (UINT)i, pipMode == SourceMode::Discovered && pipPID == stream.processId);
                }
            }
        }
    }

    CustomMenu* audioSubMenu = menu->AddSubMenu(L"Audio Source");
    if (audioSubMenu) {
        audioSubMenu->AddItem(L"None", ID_AUDIO_DEVICE_NONE, g_currentAudioDevice == ID_AUDIO_DEVICE_NONE);
        if (!g_captureDeviceNames.empty()) {
            audioSubMenu->AddSeparator();
            for (size_t i = 0; i < g_captureDeviceNames.size(); ++i) {
                UINT id = ID_AUDIO_CAPTURE_FIRST + (UINT)i;
                std::wstring deviceName = g_captureDeviceNames[i];
                if (deviceName.length() > 32) {
                    deviceName = deviceName.substr(0, 32) + L"...";
                }
                audioSubMenu->AddItem(deviceName, id, g_currentAudioDevice == id);
            }
        }
    }

    menu->AddSeparator();
    menu->AddItem(L"About", ID_TRAY_ABOUT);
    menu->AddItem(L"Exit", ID_TRAY_EXIT);
    
    SetForegroundWindow(hwnd);
    
    int menuWidth = menu->GetWidth();
    int menuHeight = menu->CalculateHeight();
    HMONITOR hMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(hMonitor, &mi);
    int x = pt.x;
    int y = pt.y;
    if (x + menuWidth > mi.rcWork.right) x = pt.x - menuWidth;
    if (y + menuHeight > mi.rcWork.bottom) y = pt.y - menuHeight;

    menu->Show(x, y, g_onIdle);
}

LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        if (FAILED(InitD3D(hwnd)) || FAILED(LoadAssets())) {
            MessageBox(hwnd, L"Failed to initialize D3D for preview.", L"Error", MB_OK | MB_ICONERROR);
            return -1;
        }
        g_hTelemetryLabel = CreateWindowW(L"STATIC", L"Status: Initializing...",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            0, 0, 640, 20,
            hwnd, (HMENU)IDC_TELEMETRY_LABEL, g_instance, NULL);
        break;
    case WM_SIZE:
        if (g_swapChain) {
            UINT width = LOWORD(lParam);
            UINT height = HIWORD(lParam);
            if (width == 0 || height == 0) break;
            if(g_context) g_context->OMSetRenderTargets(0, 0, 0); 
            g_rtv.Reset();
            g_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
            ComPtr<ID3D11Texture2D> pBuffer;
            g_swapChain->GetBuffer(0, IID_PPV_ARGS(&pBuffer));
            g_device->CreateRenderTargetView(pBuffer.Get(), NULL, &g_rtv);
            if(g_hTelemetryLabel) {
                SetWindowPos(g_hTelemetryLabel, NULL, 0, 0, width, 20, SWP_NOZORDER);
            }
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        CleanupD3D();
        g_hPreviewWnd = NULL;
        g_hTelemetryLabel = NULL;
        break;
    default:
        return DefWindowProc(hwnd, message, wParam, lParam);
    }
    return 0;
}

INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    static HFONT hLinkFont = NULL;

    UNREFERENCED_PARAMETER(lParam);
    switch (message) {
    case WM_INITDIALOG:
        {
            VirtuaCam::Utils::Win32::CenterWindowRelativeToCursor(hDlg);
            
            HWND hUrl = GetDlgItem(hDlg, IDC_URL_LINK);
            HFONT hOrigFont = (HFONT)SendMessage(hUrl, WM_GETFONT, 0, 0);
            LOGFONT lf;
            GetObject(hOrigFont, sizeof(LOGFONT), &lf);

            lf.lfUnderline = TRUE;
            hLinkFont = CreateFontIndirect(&lf);
            SendMessage(hUrl, WM_SETFONT, (WPARAM)hLinkFont, TRUE);
        }
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        if (LOWORD(wParam) == IDC_URL_LINK) {
             ShellExecuteW(NULL, L"open", L"https://github.com/MansfieldPlumbing/VirtuaCam", NULL, NULL, SW_SHOWNORMAL);
             return (INT_PTR)TRUE;
        }
        break;
    
    case WM_CTLCOLORSTATIC:
        {
            HDC hdcStatic = (HDC)wParam;
            if (GetDlgCtrlID((HWND)lParam) == IDC_URL_LINK) {
                SetTextColor(hdcStatic, RGB(0, 102, 204));
                SetBkMode(hdcStatic, TRANSPARENT);
                return (INT_PTR)GetStockObject(NULL_BRUSH);
            }
        }
        break;

    case WM_SETCURSOR:
        if (GetDlgCtrlID((HWND)wParam) == IDC_URL_LINK) {
            SetCursor(LoadCursor(NULL, IDC_HAND));
            return TRUE;
        }
        break;

    case WM_DESTROY:
        if (hLinkFont) {
            DeleteObject(hLinkFont);
            hLinkFont = NULL;
        }
        break;
    }
    return (INT_PTR)FALSE;
}

void AddTrayIcon(HWND hwnd, bool add) {
    NOTIFYICONDATA nid = { sizeof(nid) };
    nid.hWnd = hwnd;
    nid.uID = 1;
    if (add) {
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_APP_TRAY_MSG;
        nid.hIcon = (HICON)LoadImage(g_instance, MAKEINTRESOURCE(IDI_VIRTUACAM), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        wcscpy_s(nid.szTip, L"VirtuaCam");
        Shell_NotifyIcon(NIM_ADD, &nid);
    } else {
        Shell_NotifyIcon(NIM_DELETE, &nid);
    }
}

void CreatePreviewWindow() {
    if (g_hPreviewWnd && IsWindow(g_hPreviewWnd)) {
        ShowWindow(g_hPreviewWnd, SW_SHOW);
        SetForegroundWindow(g_hPreviewWnd);
        return;
    }
    RECT rc = { 0, 0, 640, 360 };
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX, FALSE);
    g_hPreviewWnd = CreateWindowW(PREVIEW_WINDOW_CLASS, L"VirtuaCam Preview",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, g_instance, NULL);
    if (g_hPreviewWnd) {
        VirtuaCam::Utils::Win32::CenterWindowRelativeToCursor(g_hPreviewWnd);
        ShowWindow(g_hPreviewWnd, SW_SHOW);
        UpdateWindow(g_hPreviewWnd);
    }
}

void UpdateTelemetry(BrokerState currentState) {
    if (!g_hTelemetryLabel) return;
    static BrokerState lastState = (BrokerState)-1;
    if (currentState != lastState) {
        lastState = currentState;
        switch (currentState) {
        case BrokerState::Searching:
            SetWindowText(g_hTelemetryLabel, L"Status: Searching for Producer...");
            break;
        case BrokerState::Connected:
            SetWindowText(g_hTelemetryLabel, L"Status: Connected to Producer");
            break;
        case BrokerState::Failed:
            SetWindowText(g_hTelemetryLabel, L"Status: Disconnected / No Producer Found");
            break;
        }
    }
}

HRESULT InitD3D(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    UINT width = rc.right - rc.left;
    UINT height = rc.bottom - rc.top;

    ComPtr<IDXGIDevice> dxgiDevice;
    RETURN_IF_FAILED(g_device.As(&dxgiDevice));
    ComPtr<IDXGIAdapter> dxgiAdapter;
    RETURN_IF_FAILED(dxgiDevice->GetAdapter(&dxgiAdapter));
    ComPtr<IDXGIFactory> dxgiFactory;
    RETURN_IF_FAILED(dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory)));

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = width;
    scd.BufferDesc.Height = height;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    
    RETURN_IF_FAILED(dxgiFactory->CreateSwapChain(g_device.Get(), &scd, &g_swapChain));

    ComPtr<ID3D11Texture2D> pBuffer;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&pBuffer));
    RETURN_IF_FAILED(g_device->CreateRenderTargetView(pBuffer.Get(), NULL, &g_rtv));
    
    return S_OK;
}

void CleanupD3D() {
    if(g_context) g_context->ClearState();
    g_rtv.Reset();
    g_vs.Reset();
    g_ps.Reset();
    g_sampler.Reset();
    g_swapChain.Reset();
}

HRESULT LoadAssets() {
    ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;
    HRESULT hr = D3DCompile(g_vertexShaderHLSL, strlen(g_vertexShaderHLSL), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &errorBlob);
    if (FAILED(hr) && errorBlob) OutputDebugStringA((char*)errorBlob->GetBufferPointer());
    RETURN_IF_FAILED(hr);

    hr = D3DCompile(g_pixelShaderHLSL, strlen(g_pixelShaderHLSL), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &errorBlob);
    if (FAILED(hr) && errorBlob) OutputDebugStringA((char*)errorBlob->GetBufferPointer());
    RETURN_IF_FAILED(hr);

    RETURN_IF_FAILED(g_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_vs));
    RETURN_IF_FAILED(g_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_ps));

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP; sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP; sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    RETURN_IF_FAILED(g_device->CreateSamplerState(&sd, &g_sampler));
    return S_OK;
}

void RenderPreviewFrame(HWND hwnd) {
    if (!g_rtv) return;

    ComPtr<ID3D11ShaderResourceView> previewSRV;
    if (g_pfnGetSharedTextureHandle && g_device1) {
        HANDLE sharedHandle = g_pfnGetSharedTextureHandle();
        if (sharedHandle) {
            ComPtr<ID3D11Texture2D> sharedTex;
            if (SUCCEEDED(g_device1->OpenSharedResource1(sharedHandle, IID_PPV_ARGS(&sharedTex)))) {
                g_device->CreateShaderResourceView(sharedTex.Get(), nullptr, &previewSRV);
            }
        }
    }
    
    const float clearColor[] = { 0.1f, 0.1f, 0.1f, 1.0f };
    g_context->ClearRenderTargetView(g_rtv.Get(), clearColor);

    if (previewSRV) {
        RECT rc; GetClientRect(hwnd, &rc);
        D3D11_VIEWPORT vp = { 0, 0, (float)rc.right, (float)rc.bottom, 0, 1 };
        if (vp.Width <= 0 || vp.Height <= 0) {
            g_swapChain->Present(1, 0);
            return;
        }
        g_context->RSSetViewports(1, &vp);
        g_context->OMSetRenderTargets(1, g_rtv.GetAddressOf(), nullptr);
        g_context->VSSetShader(g_vs.Get(), nullptr, 0);
        g_context->PSSetShader(g_ps.Get(), nullptr, 0);
        g_context->PSSetShaderResources(0, 1, previewSRV.GetAddressOf());
        g_context->PSSetSamplers(0, 1, g_sampler.GetAddressOf());
        g_context->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_context->Draw(3, 0);
    }
    
    HRESULT hr = g_swapChain->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
    {
        DestroyWindow(hwnd);
    }
}