#include "pch.h"
#include "App.h"
#include "UI.h"
#include "Menu.h"
#include "Tools.h"
#include "Formats.h"
#include "Discovery.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <dwmapi.h>
#include <map>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dwmapi.lib")

using namespace Microsoft::WRL;

extern const VirtuaCam::Discovery* GetGlobalDiscovery();
extern void InformBroker();
void ShutdownSystem();

extern const SourceState& GetMainSourceState();
extern void SetSourceMode(SourceMode newMode, DWORD_PTR context);

extern const SourceState& GetPipSourceState(PipPosition pos);
extern void SetPipSource(PipPosition pos, SourceMode newMode, DWORD_PTR context);

extern bool GetPipTlEnabled();
extern bool GetPipTrEnabled();
extern bool GetPipBlEnabled();
extern void TogglePipTl();
extern void TogglePipTr();
extern void TogglePipBl();

struct EnumWindowsData { std::vector<CapturableWindow>* windows; };
BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    EnumWindowsData* data = reinterpret_cast<EnumWindowsData*>(lParam);
    if (!IsWindowVisible(hwnd) || GetWindowTextLength(hwnd) == 0 || (GetWindowLong(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) {
        return TRUE;
    }
    BOOL isCloaked = FALSE;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &isCloaked, sizeof(isCloaked));
    if (isCloaked) return TRUE;

    wchar_t title[256];
    GetWindowTextW(hwnd, title, ARRAYSIZE(title));
    data->windows->push_back({ hwnd, title });
    return TRUE;
}
std::vector<CapturableWindow> EnumerateWindows() {
    EnumWindowsData data;
    data.windows = new std::vector<CapturableWindow>();
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&data));
    std::vector<CapturableWindow> result = *data.windows;
    delete data.windows;
    return result;
}

std::vector<std::wstring> EnumerateCameras() {
    std::vector<std::wstring> cameraNames;
    ComPtr<IMFAttributes> pAttributes;
    if (FAILED(MFCreateAttributes(&pAttributes, 1))) return cameraNames;
    if (FAILED(pAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) return cameraNames;

    UINT32 count = 0;
    IMFActivate** devices = nullptr;
    if (FAILED(MFEnumDeviceSources(pAttributes.Get(), &devices, &count))) return cameraNames;

    for (UINT32 i = 0; i < count; i++) {
        wchar_t* friendlyName = nullptr;
        UINT32 nameLength = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &friendlyName, &nameLength))) {
            std::wstring name(friendlyName);
            if (name.find(L"VirtuaCam") == std::wstring::npos) {
                cameraNames.push_back(name);
            }
            CoTaskMemFree(friendlyName);
        }
        devices[i]->Release();
    }
    CoTaskMemFree(devices);
    return cameraNames;
}

static HINSTANCE g_instance;
static HWND g_hMainWnd = NULL;
static HWND g_hPreviewWnd = NULL;
static HWND g_hTelemetryLabel = NULL;
static WCHAR g_windowClass[MAX_LOADSTRING];
static std::function<void(int)> g_audioSelectionCallback;
static std::vector<std::wstring> g_captureDeviceNames;
static int g_currentAudioDevice = ID_AUDIO_DEVICE_NONE;
static PFN_GetSharedTexture g_pfnGetSharedTexture = nullptr;
static std::function<void()> g_onIdle;

static ComPtr<ID3D11Device> g_device;
static ComPtr<ID3D11DeviceContext> g_context;
static ComPtr<IDXGISwapChain> g_swapChain;
static ComPtr<ID3D11RenderTargetView> g_rtv;
static ComPtr<ID3D11VertexShader> g_vs;
static ComPtr<ID3D11PixelShader> g_ps;
static ComPtr<ID3D11SamplerState> g_sampler;
static ComPtr<ID3D11ShaderResourceView> g_previewSRV;
static ComPtr<ID3D11Texture2D> g_uiSideTexture;

static std::map<UINT, HWND> g_mainSourceWindowMap;
static std::map<UINT, HWND> g_pipTlWindowMap;
static std::map<UINT, HWND> g_pipTrWindowMap;
static std::map<UINT, HWND> g_pipBlWindowMap;
static std::map<UINT, HWND> g_pipWindowMap;

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

void UI_Initialize(HINSTANCE instance, HWND& outMainWnd, PFN_GetSharedTexture pfnGetSharedTexture) {
    g_instance = instance;
    g_pfnGetSharedTexture = pfnGetSharedTexture;
    LoadStringW(instance, IDC_VIRTUACAM, g_windowClass, MAX_LOADSTRING);
    MyRegisterClass(instance);

    g_hMainWnd = CreateWindowEx(
        0,
        g_windowClass,
        L"VirtuaCam Message Window",
        0, 0, 0, 0, 0,
        HWND_MESSAGE,
        nullptr,
        instance,
        nullptr
    );

    if (g_hMainWnd) {
        AddTrayIcon(g_hMainWnd, true);
    }
    outMainWnd = g_hMainWnd;
}

void UI_RunMessageLoop(std::function<void()> onIdle) {
    g_onIdle = onIdle;
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            if (g_onIdle) g_onIdle();
            if (g_hPreviewWnd && IsWindow(g_hPreviewWnd)) {
                RenderPreviewFrame(g_hPreviewWnd);
            } else {
                Sleep(10);
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
}

void UI_SetAudioSelectionCallback(std::function<void(int)> callback) {
    g_audioSelectionCallback = callback;
}

ATOM MyRegisterClass(HINSTANCE instance) {
    WNDCLASSEXW wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEX); wcex.lpfnWndProc = WndProc;
    wcex.hInstance = instance; wcex.lpszClassName = g_windowClass;
    wcex.hIcon = LoadIcon(instance, MAKEINTRESOURCE(IDI_VIRTUACAM));
    wcex.hIconSm = LoadIcon(instance, MAKEINTRESOURCE(IDI_SMALL));
    RegisterClassExW(&wcex);

    WNDCLASSEXW wcexPreview = {};
    wcexPreview.cbSize = sizeof(WNDCLASSEX); wcexPreview.style = CS_HREDRAW | CS_VREDRAW;
    wcexPreview.lpfnWndProc = PreviewWndProc; wcexPreview.hInstance = instance;
    wcexPreview.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcexPreview.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcexPreview.lpszClassName = PREVIEW_WINDOW_CLASS;
    wcexPreview.hIcon = LoadIcon(instance, MAKEINTRESOURCE(IDI_VIRTUACAM));
    wcexPreview.hIconSm = LoadIcon(instance, MAKEINTRESOURCE(IDI_SMALL));
    return RegisterClassExW(&wcexPreview);
}

void HandlePipCommand(PipPosition pos, UINT id) {
    const auto* discovery = GetGlobalDiscovery();
    const auto& streams = discovery ? discovery->GetDiscoveredStreams() : std::vector<VirtuaCam::DiscoveredSharedStream>();
    int streamIndex;

    switch (pos) {
    case PipPosition::TL:
        if (id >= ID_PIP_TL_DISCOVERED_FIRST) {
            streamIndex = id - ID_PIP_TL_DISCOVERED_FIRST;
            if (streamIndex >= 0 && (size_t)streamIndex < streams.size()) SetPipSource(pos, SourceMode::Discovered, streams[streamIndex].processId);
        } else if (id >= ID_PIP_TL_WINDOW_FIRST) {
            if (g_pipTlWindowMap.count(id)) SetPipSource(pos, SourceMode::Window, reinterpret_cast<DWORD_PTR>(g_pipTlWindowMap[id]));
        } else if (id >= ID_PIP_TL_CAMERA_FIRST) {
            SetPipSource(pos, SourceMode::Camera, id - ID_PIP_TL_CAMERA_FIRST);
        } else if (id == ID_PIP_TL_CONSUMER) {
            SetPipSource(pos, SourceMode::Consumer, 0);
        } else if (id == ID_PIP_TL_OFF) {
            SetPipSource(pos, SourceMode::Off, 0);
        }
        break;

    case PipPosition::TR:
        if (id >= ID_PIP_TR_DISCOVERED_FIRST) {
            streamIndex = id - ID_PIP_TR_DISCOVERED_FIRST;
            if (streamIndex >= 0 && (size_t)streamIndex < streams.size()) SetPipSource(pos, SourceMode::Discovered, streams[streamIndex].processId);
        } else if (id >= ID_PIP_TR_WINDOW_FIRST) {
            if (g_pipTrWindowMap.count(id)) SetPipSource(pos, SourceMode::Window, reinterpret_cast<DWORD_PTR>(g_pipTrWindowMap[id]));
        } else if (id >= ID_PIP_TR_CAMERA_FIRST) {
            SetPipSource(pos, SourceMode::Camera, id - ID_PIP_TR_CAMERA_FIRST);
        } else if (id == ID_PIP_TR_CONSUMER) {
            SetPipSource(pos, SourceMode::Consumer, 0);
        } else if (id == ID_PIP_TR_OFF) {
            SetPipSource(pos, SourceMode::Off, 0);
        }
        break;

    case PipPosition::BL:
        if (id >= ID_PIP_BL_DISCOVERED_FIRST) {
            streamIndex = id - ID_PIP_BL_DISCOVERED_FIRST;
            if (streamIndex >= 0 && (size_t)streamIndex < streams.size()) SetPipSource(pos, SourceMode::Discovered, streams[streamIndex].processId);
        } else if (id >= ID_PIP_BL_WINDOW_FIRST) {
            if (g_pipBlWindowMap.count(id)) SetPipSource(pos, SourceMode::Window, reinterpret_cast<DWORD_PTR>(g_pipBlWindowMap[id]));
        } else if (id >= ID_PIP_BL_CAMERA_FIRST) {
            SetPipSource(pos, SourceMode::Camera, id - ID_PIP_BL_CAMERA_FIRST);
        } else if (id == ID_PIP_BL_CONSUMER) {
            SetPipSource(pos, SourceMode::Consumer, 0);
        } else if (id == ID_PIP_BL_OFF) {
            SetPipSource(pos, SourceMode::Off, 0);
        }
        break;

    case PipPosition::BR:
        if (id >= ID_PIP_DISCOVERED_FIRST) {
            streamIndex = id - ID_PIP_DISCOVERED_FIRST;
            if (streamIndex >= 0 && (size_t)streamIndex < streams.size()) SetPipSource(pos, SourceMode::Discovered, streams[streamIndex].processId);
        } else if (id >= ID_PIP_WINDOW_FIRST) {
            if (g_pipWindowMap.count(id)) SetPipSource(pos, SourceMode::Window, reinterpret_cast<DWORD_PTR>(g_pipWindowMap[id]));
        } else if (id >= ID_PIP_CAMERA_FIRST) {
            SetPipSource(pos, SourceMode::Camera, id - ID_PIP_CAMERA_FIRST);
        } else if (id == ID_PIP_CONSUMER) {
            SetPipSource(pos, SourceMode::Consumer, 0);
        } else if (id == ID_PIP_OFF) {
            SetPipSource(pos, SourceMode::Off, 0);
        }
        break;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_TIMER:
        if (wParam == 1) InformBroker();
        break;
    case WM_APP_TRAY_MSG:
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) ShowContextMenu(hwnd);
        else if (lParam == WM_LBUTTONDBLCLK) CreatePreviewWindow();
        break;
    case WM_APP_MENU_COMMAND:
    {
        UINT id = (UINT)wParam;
        
        if (id != 0)
        {
            if (id == ID_SETTINGS_PIP_TL) TogglePipTl();
            else if (id == ID_SETTINGS_PIP_TR) TogglePipTr();
            else if (id == ID_SETTINGS_PIP_BL) TogglePipBl();
            else if (id >= ID_PIP_OFF) HandlePipCommand(PipPosition::BR, id);
            else if (id >= ID_PIP_BL_OFF) HandlePipCommand(PipPosition::BL, id);
            else if (id >= ID_PIP_TR_OFF) HandlePipCommand(PipPosition::TR, id);
            else if (id >= ID_PIP_TL_OFF) HandlePipCommand(PipPosition::TL, id);
            else if (id >= ID_SOURCE_OFF) {
                if (id == ID_SOURCE_OFF) SetSourceMode(SourceMode::Off, 0);
                else if (id == ID_SOURCE_CONSUMER) SetSourceMode(SourceMode::Consumer, 0);
                else if (id >= ID_SOURCE_CAMERA_FIRST && id < ID_SOURCE_WINDOW_FIRST) {
                    SetSourceMode(SourceMode::Camera, id - ID_SOURCE_CAMERA_FIRST);
                }
                else if (id >= ID_SOURCE_WINDOW_FIRST && id < ID_SOURCE_DISCOVERED_FIRST) {
                    if (g_mainSourceWindowMap.count(id)) {
                        SetSourceMode(SourceMode::Window, reinterpret_cast<DWORD_PTR>(g_mainSourceWindowMap[id]));
                    }
                }
                else if (id >= ID_SOURCE_DISCOVERED_FIRST) {
                    const auto* discovery = GetGlobalDiscovery();
                    if (discovery) {
                        int index = id - ID_SOURCE_DISCOVERED_FIRST;
                        const auto& streams = discovery->GetDiscoveredStreams();
                        if (index >= 0 && (size_t)index < streams.size()) {
                            SetSourceMode(SourceMode::Discovered, streams[index].processId);
                        }
                    }
                }
            }
            else if (id >= ID_AUDIO_DEVICE_NONE) {
                g_currentAudioDevice = id;
                if (g_audioSelectionCallback) g_audioSelectionCallback(id);
            }
            else if (id == ID_TRAY_PREVIEW_WINDOW) CreatePreviewWindow();
            else if (id == ID_TRAY_ABOUT) DialogBox(g_instance, MAKEINTRESOURCE(IDD_ABOUTBOX), hwnd, About);
            else if (id == ID_TRAY_EXIT) DestroyWindow(hwnd);
        }
        break;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, message, wParam, lParam);
    }
    return 0;
}

void BuildSourceSubMenu(CustomMenu* subMenu, bool isPip, PipPosition pos = PipPosition::BR) {
    UINT id_off, id_consumer, id_camera_first, id_window_first, id_discovered_first;
    std::map<UINT, HWND>* windowMap = nullptr;
    const SourceState* state = nullptr;

    if (!isPip) {
        id_off = ID_SOURCE_OFF; id_consumer = ID_SOURCE_CONSUMER; id_camera_first = ID_SOURCE_CAMERA_FIRST;
        id_window_first = ID_SOURCE_WINDOW_FIRST; id_discovered_first = ID_SOURCE_DISCOVERED_FIRST;
        windowMap = &g_mainSourceWindowMap; state = &GetMainSourceState();
    }
    else {
        switch (pos) {
        case PipPosition::TL:
            id_off = ID_PIP_TL_OFF; id_consumer = ID_PIP_TL_CONSUMER; id_camera_first = ID_PIP_TL_CAMERA_FIRST;
            id_window_first = ID_PIP_TL_WINDOW_FIRST; id_discovered_first = ID_PIP_TL_DISCOVERED_FIRST;
            windowMap = &g_pipTlWindowMap; state = &GetPipSourceState(PipPosition::TL);
            break;
        case PipPosition::TR:
            id_off = ID_PIP_TR_OFF; id_consumer = ID_PIP_TR_CONSUMER; id_camera_first = ID_PIP_TR_CAMERA_FIRST;
            id_window_first = ID_PIP_TR_WINDOW_FIRST; id_discovered_first = ID_PIP_TR_DISCOVERED_FIRST;
            windowMap = &g_pipTrWindowMap; state = &GetPipSourceState(PipPosition::TR);
            break;
        case PipPosition::BL:
            id_off = ID_PIP_BL_OFF; id_consumer = ID_PIP_BL_CONSUMER; id_camera_first = ID_PIP_BL_CAMERA_FIRST;
            id_window_first = ID_PIP_BL_WINDOW_FIRST; id_discovered_first = ID_PIP_BL_DISCOVERED_FIRST;
            windowMap = &g_pipBlWindowMap; state = &GetPipSourceState(PipPosition::BL);
            break;
        case PipPosition::BR:
            id_off = ID_PIP_OFF; id_consumer = ID_PIP_CONSUMER; id_camera_first = ID_PIP_CAMERA_FIRST;
            id_window_first = ID_PIP_WINDOW_FIRST; id_discovered_first = ID_PIP_DISCOVERED_FIRST;
            windowMap = &g_pipWindowMap; state = &GetPipSourceState(PipPosition::BR);
            break;
        }
    }

    windowMap->clear();

    subMenu->AddItem(L"Off", id_off, state->mode == SourceMode::Off);
    subMenu->AddItem(isPip ? L"Discovery" : L"Auto-Discovery Grid", id_consumer, state->mode == SourceMode::Consumer);
    subMenu->AddSeparator();

    auto cameras = EnumerateCameras();
    if (!cameras.empty()) {
        for (size_t i = 0; i < cameras.size(); ++i) {
            std::wstring name = cameras[i];
            if (name.length() > 32) name = name.substr(0, 29) + L"...";
            subMenu->AddItem(name, id_camera_first + (UINT)i, state->mode == SourceMode::Camera && state->cameraIndex == (int)i);
        }
        subMenu->AddSeparator();
    }

    auto windows = EnumerateWindows();
    if (!windows.empty()) {
        for (size_t i = 0; i < windows.size() && i < (ID_SOURCE_DISCOVERED_FIRST - ID_SOURCE_WINDOW_FIRST); ++i) {
            UINT menuId = id_window_first + (UINT)i;
            (*windowMap)[menuId] = windows[i].hwnd;
            std::wstring title = windows[i].title;
            if (title.length() > 32) title = title.substr(0, 29) + L"...";
            subMenu->AddItem(title, menuId, state->mode == SourceMode::Window && state->hwnd == windows[i].hwnd);
        }
    }

    const auto* discovery = GetGlobalDiscovery();
    if (discovery && !discovery->GetDiscoveredStreams().empty()) {
        bool separatorAdded = false;
        int discoveredCount = 0;
        for (size_t i = 0; i < discovery->GetDiscoveredStreams().size(); ++i) {
            const auto& stream = discovery->GetDiscoveredStreams()[i];
            if (stream.processName != L"VirtuaCamProcess.exe") {
                if (!separatorAdded && (!windows.empty() || discoveredCount > 0)) {
                    subMenu->AddSeparator();
                    separatorAdded = true;
                }
                std::wstring label = stream.processName + L" (PID: " + std::to_wstring(stream.processId) + L")";
                if (label.length() > 32) label = label.substr(0, 29) + L"...";
                subMenu->AddItem(label, id_discovered_first + (UINT)i, state->mode == SourceMode::Discovered && state->pid == stream.processId);
                discoveredCount++;
            }
        }
    }
}

void ShowContextMenu(HWND hwnd) {
    CustomMenu::CloseAllMenus();
    POINT pt; GetCursorPos(&pt);

    auto menu = new CustomMenu(hwnd, g_instance);
    menu->AddItem(L"Show Preview", ID_TRAY_PREVIEW_WINDOW);
    menu->AddSeparator();

    BuildSourceSubMenu(menu->AddSubMenu(L"Source"), false);

    if (GetPipTlEnabled()) {
        BuildSourceSubMenu(menu->AddSubMenu(L"PIP (Top Left)"), true, PipPosition::TL);
    }
    if (GetPipTrEnabled()) {
        BuildSourceSubMenu(menu->AddSubMenu(L"PIP (Top Right)"), true, PipPosition::TR);
    }
    if (GetPipBlEnabled()) {
        BuildSourceSubMenu(menu->AddSubMenu(L"PIP (Bottom Left)"), true, PipPosition::BL);
    }

    BuildSourceSubMenu(menu->AddSubMenu(L"Picture-in-Picture"), true, PipPosition::BR);

    CustomMenu* audioSubMenu = menu->AddSubMenu(L"Audio Source");
    if (audioSubMenu) {
        audioSubMenu->AddItem(L"None", ID_AUDIO_DEVICE_NONE, g_currentAudioDevice == ID_AUDIO_DEVICE_NONE);
        if (!g_captureDeviceNames.empty()) {
            audioSubMenu->AddSeparator();
            for (size_t i = 0; i < g_captureDeviceNames.size(); ++i) {
                UINT id = ID_AUDIO_CAPTURE_FIRST + (UINT)i;
                audioSubMenu->AddItem(g_captureDeviceNames[i], id, g_currentAudioDevice == id);
            }
        }
    }

    menu->AddSeparator();

    CustomMenu* settingsMenu = menu->AddSubMenu(L"Settings");
    if (settingsMenu) {
        settingsMenu->AddItem(L"PIP Top Left", ID_SETTINGS_PIP_TL, GetPipTlEnabled());
        settingsMenu->AddItem(L"PIP Top Right", ID_SETTINGS_PIP_TR, GetPipTrEnabled());
        settingsMenu->AddItem(L"PIP Bottom Left", ID_SETTINGS_PIP_BL, GetPipBlEnabled());
    }

    menu->AddItem(L"About", ID_TRAY_ABOUT);
    menu->AddItem(L"Exit", ID_TRAY_EXIT);

    int menuWidth = menu->GetCalculatedWidth();
    int menuHeight = menu->GetCalculatedHeight();

    HMONITOR hMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(hMonitor, &mi);

    int x = pt.x;
    int y = pt.y;
    if (x + menuWidth > mi.rcWork.right) {
        x = pt.x - menuWidth;
    }
    if (y + menuHeight > mi.rcWork.bottom) {
        y = pt.y - menuHeight;
    }

    menu->Show(x, y);
}

LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
    {
        BOOL useDarkMode = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));
        if (FAILED(InitD3D(hwnd)) || FAILED(LoadAssets())) {
            MessageBox(hwnd, L"Failed to initialize D3D for preview.", L"Error", MB_OK | MB_ICONERROR);
            return -1;
        }
        g_hTelemetryLabel = CreateWindowW(L"STATIC", L"Status: Initializing...", WS_CHILD | WS_VISIBLE | SS_CENTER, 0, 0, 640, 20, hwnd, (HMENU)IDC_TELEMETRY_LABEL, g_instance, NULL);
        if (!g_pfnGetSharedTexture) {
            MessageBox(hwnd, L"Broker's GetSharedTexture function not available.", L"Error", MB_OK | MB_ICONERROR);
            return -1;
        }
        break;
    }
    case WM_SIZE:
        if (g_swapChain) {
            UINT width = LOWORD(lParam); UINT height = HIWORD(lParam);
            if (width == 0 || height == 0) break;
            if (g_context) g_context->OMSetRenderTargets(0, 0, 0);
            g_rtv.Reset();
            HRESULT hr = g_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
            if (FAILED(hr)) return 0;
            ComPtr<ID3D11Texture2D> pBuffer;
            if (FAILED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&pBuffer)))) return 0;
            if (FAILED(g_device->CreateRenderTargetView(pBuffer.Get(), NULL, &g_rtv))) return 0;
            if (g_hTelemetryLabel) SetWindowPos(g_hTelemetryLabel, NULL, 0, 0, width, 20, SWP_NOZORDER);
        }
        break;
    case WM_CLOSE: DestroyWindow(hwnd); break;
    case WM_DESTROY: CleanupD3D(); g_hPreviewWnd = NULL; g_hTelemetryLabel = NULL; break;
    default: return DefWindowProc(hwnd, message, wParam, lParam);
    }
    return 0;
}

INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    UNREFERENCED_PARAMETER(lParam);
    switch (message) {
    case WM_INITDIALOG: CenterWindow(hDlg, true); return (INT_PTR)TRUE;
    case WM_COMMAND: if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) { EndDialog(hDlg, LOWORD(wParam)); return (INT_PTR)TRUE; } break;
    }
    return (INT_PTR)FALSE;
}

void AddTrayIcon(HWND hwnd, bool add) {
    NOTIFYICONDATA nid = { sizeof(nid) }; nid.hWnd = hwnd; nid.uID = 1;
    if (add) {
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_APP_TRAY_MSG;
        nid.hIcon = (HICON)LoadImage(g_instance, MAKEINTRESOURCE(IDI_VIRTUACAM), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        wcscpy_s(nid.szTip, L"VirtuaCam");
        Shell_NotifyIcon(NIM_ADD, &nid);
    } else {
        Shell_NotifyIcon(NIM_DELETE, &nid);
    }
}

void CreatePreviewWindow() {
    if (g_hPreviewWnd && IsWindow(g_hPreviewWnd)) {
        ShowWindow(g_hPreviewWnd, SW_SHOW); SetForegroundWindow(g_hPreviewWnd); return;
    }
    RECT rc = { 0, 0, 640, 360 };
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX, FALSE);
    g_hPreviewWnd = CreateWindowW(PREVIEW_WINDOW_CLASS, L"VirtuaCam Preview", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, g_instance, NULL);
    if (g_hPreviewWnd) { CenterWindow(g_hPreviewWnd, true); ShowWindow(g_hPreviewWnd, SW_SHOW); UpdateWindow(g_hPreviewWnd); }
}

void UpdateTelemetry(BrokerState currentState) {
    if (!g_hTelemetryLabel) return;
    static BrokerState lastState = (BrokerState)-1;
    if (currentState != lastState) {
        lastState = currentState;
        switch (currentState) {
        case BrokerState::Searching: SetWindowText(g_hTelemetryLabel, L"Status: Searching for Producer..."); break;
        case BrokerState::Connected: SetWindowText(g_hTelemetryLabel, L"Status: Connected to Producer"); break;
        case BrokerState::Failed: SetWindowText(g_hTelemetryLabel, L"Status: Disconnected / No Producer Found"); break;
        }
    }
}

HRESULT InitD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2; scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1; scd.Windowed = TRUE; scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &scd, &g_swapChain, &g_device, nullptr, &g_context);
    if (SUCCEEDED(hr)) {
        ComPtr<ID3D11Texture2D> pBuffer; g_swapChain->GetBuffer(0, IID_PPV_ARGS(&pBuffer));
        g_device->CreateRenderTargetView(pBuffer.Get(), NULL, &g_rtv);
    }
    return hr;
}

void CleanupD3D() {
    if (g_context) g_context->ClearState();
    g_rtv.Reset(); g_swapChain.Reset(); g_context.Reset(); g_device.Reset();
    g_vs.Reset(); g_ps.Reset(); g_sampler.Reset(); g_previewSRV.Reset();
    g_uiSideTexture.Reset();
}

HRESULT LoadAssets() {
    ComPtr<ID3DBlob> vsBlob, psBlob;
    RETURN_IF_FAILED(D3DCompile(g_vertexShaderHLSL, strlen(g_vertexShaderHLSL), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, nullptr));
    RETURN_IF_FAILED(D3DCompile(g_pixelShaderHLSL, strlen(g_pixelShaderHLSL), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, nullptr));
    RETURN_IF_FAILED(g_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_vs));
    RETURN_IF_FAILED(g_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_ps));
    D3D11_SAMPLER_DESC sd = {}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP; sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP; sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    RETURN_IF_FAILED(g_device->CreateSamplerState(&sd, &g_sampler));
    return S_OK;
}

void RenderPreviewFrame(HWND hwnd) {
    if (!g_rtv || !g_device || !g_context) return;

    if (!g_previewSRV && g_device)
    {
        ComPtr<ID3D11Device1> device1;
        if (SUCCEEDED(g_device.As(&device1)))
        {
            wil::unique_handle sharedHandle(GetHandleFromName(L"Global\\VirtuaCast_Broker_Texture"));
            if (sharedHandle)
            {
                if (SUCCEEDED(device1->OpenSharedResource1(sharedHandle.get(), IID_PPV_ARGS(&g_uiSideTexture))))
                {
                    g_device->CreateShaderResourceView(g_uiSideTexture.Get(), nullptr, &g_previewSRV);
                }
            }
        }
    }

    const float clearColor[] = { 0.1f, 0.1f, 0.1f, 1.0f };
    g_context->ClearRenderTargetView(g_rtv.Get(), clearColor);

    if (g_previewSRV) {
        RECT rc; GetClientRect(hwnd, &rc);
        D3D11_VIEWPORT vp = { 0, 0, (float)rc.right, (float)rc.bottom, 0, 1 };
        if (vp.Width <= 0 || vp.Height <= 0)
        {
            if (g_swapChain) g_swapChain->Present(1, 0);
            return;
        }
        g_context->RSSetViewports(1, &vp);
        g_context->OMSetRenderTargets(1, g_rtv.GetAddressOf(), nullptr);
        g_context->VSSetShader(g_vs.Get(), nullptr, 0); g_context->PSSetShader(g_ps.Get(), nullptr, 0);
        g_context->PSSetShaderResources(0, 1, g_previewSRV.GetAddressOf());
        g_context->PSSetSamplers(0, 1, g_sampler.GetAddressOf());
        g_context->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_context->Draw(3, 0);
    }

    if(g_swapChain) g_swapChain->Present(1, 0);
}