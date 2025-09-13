#include "App.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
using namespace Microsoft::WRL;
std::wstring to_wstring(const std::string& s)
{
	if (s.empty()) return std::wstring();
	auto ssize = (int)s.size();
	auto wsize = MultiByteToWideChar(CP_THREAD_ACP, 0, s.data(), ssize, nullptr, 0);
	if (!wsize) return std::wstring();
	std::wstring ws;
	ws.resize(wsize);
	wsize = MultiByteToWideChar(CP_THREAD_ACP, 0, s.data(), ssize, &ws[0], wsize);
	if (!wsize) return std::wstring();
	return ws;
}
const std::wstring GUID_ToStringW(const GUID& guid)
{
	wchar_t name[64];
	std::ignore = StringFromGUID2(guid, name, _countof(name));
	return name;
}
void CenterWindow(HWND hwnd, bool useCursorPos)
{
    if (!IsWindow(hwnd)) return;
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    auto width = rc.right - rc.left;
    auto height = rc.bottom - rc.top;
    HMONITOR monitor;
    if (useCursorPos) {
        POINT pt{};
        GetCursorPos(&pt);
        monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    } else {
        monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    }
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfo(monitor, &mi)) {
        int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - width) / 2;
        int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - height) / 2;
        SetWindowPos(hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}
#define MAX_LOADSTRING 100
#define PREVIEW_WINDOW_CLASS L"VirtuaCamPreviewClass"
static GUID CLSID_VCam = { 0x3cad447d,0xf283,0x4af4,{0xa3,0xb2,0x6f,0x53,0x63,0x30,0x9f,0x52} };
typedef void (*PFN_InitializeBroker)();
typedef void (*PFN_ShutdownBroker)();
typedef void (*PFN_RenderBrokerFrame)();
typedef ID3D11Texture2D* (*PFN_GetSharedTexture)();
typedef BrokerState (*PFN_GetBrokerState)();
static HMODULE g_hBrokerDll = nullptr;
static PFN_InitializeBroker g_pfnInitializeBroker = nullptr;
static PFN_ShutdownBroker g_pfnShutdownBroker = nullptr;
static PFN_RenderBrokerFrame g_pfnRenderBrokerFrame = nullptr;
static PFN_GetSharedTexture g_pfnGetSharedTexture = nullptr;
static PFN_GetBrokerState g_pfnGetBrokerState = nullptr;
static ComPtr<ID3D11Device> g_device;
static ComPtr<ID3D11DeviceContext> g_context;
static ComPtr<IDXGISwapChain> g_swapChain;
static ComPtr<ID3D11RenderTargetView> g_rtv;
static ComPtr<ID3D11VertexShader> g_vs;
static ComPtr<ID3D11PixelShader> g_ps;
static ComPtr<ID3D11SamplerState> g_sampler;
static ComPtr<ID3D11ShaderResourceView> g_previewSRV;
static HINSTANCE _instance;
static WCHAR _title[MAX_LOADSTRING];
static WCHAR _windowClass[MAX_LOADSTRING];
static wil::com_ptr_nothrow<IMFVirtualCamera> _vcam;
static HWND g_hMainWnd = NULL;
static HWND g_hPreviewWnd = NULL;
static HWND g_hTelemetryLabel = NULL;
static BrokerState g_lastBrokerState = BrokerState::Failed;
ATOM MyRegisterClass(HINSTANCE hInstance);
HWND InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK PreviewWndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);
void RenderPreviewFrame(HWND hwnd);
HRESULT InitD3D(HWND hwnd);
void CleanupD3D();
void AddTrayIcon(HWND hwnd, bool add);
void ShowContextMenu(HWND hwnd);
void CreatePreviewWindow();
void UpdateTelemetry();
bool IsRunningAsAdmin();
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
HRESULT LoadAssets() {
    ComPtr<ID3DBlob> vsBlob, psBlob;
    D3DCompile(g_vertexShaderHLSL, strlen(g_vertexShaderHLSL), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, nullptr);
    D3DCompile(g_pixelShaderHLSL, strlen(g_pixelShaderHLSL), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, nullptr);
    g_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_vs);
    g_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_ps);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP; sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP; sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    g_device->CreateSamplerState(&sd, &g_sampler);
    return S_OK;
}
HRESULT LaunchAndLoadBroker() {
    g_hBrokerDll = LoadLibraryW(L"DirectPortBroker.dll");
    if (!g_hBrokerDll) return HRESULT_FROM_WIN32(GetLastError());
    g_pfnInitializeBroker = (PFN_InitializeBroker)GetProcAddress(g_hBrokerDll, "InitializeBroker");
    g_pfnShutdownBroker = (PFN_ShutdownBroker)GetProcAddress(g_hBrokerDll, "ShutdownBroker");
    g_pfnRenderBrokerFrame = (PFN_RenderBrokerFrame)GetProcAddress(g_hBrokerDll, "RenderBrokerFrame");
    g_pfnGetSharedTexture = (PFN_GetSharedTexture)GetProcAddress(g_hBrokerDll, "GetSharedTexture");
    g_pfnGetBrokerState = (PFN_GetBrokerState)GetProcAddress(g_hBrokerDll, "GetBrokerState");
    if (!g_pfnInitializeBroker || !g_pfnShutdownBroker || !g_pfnRenderBrokerFrame || !g_pfnGetSharedTexture || !g_pfnGetBrokerState) return E_FAIL;
    g_pfnInitializeBroker();
    return S_OK;
}
HRESULT RegisterVirtualCamera() {
    auto clsid = GUID_ToStringW(CLSID_VCam);
    RETURN_IF_FAILED_MSG(MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session, MFVirtualCameraAccess_CurrentUser, L"VirtuaCam", clsid.c_str(), nullptr, 0, &_vcam), "Failed to create virtual camera");
    RETURN_IF_FAILED_MSG(_vcam->Start(nullptr), "Cannot start VCam");
    return S_OK;
}
void ShutdownSystem() {
    if (_vcam) { _vcam->Remove(); _vcam.reset(); }
    if (g_pfnShutdownBroker) g_pfnShutdownBroker();
    if (g_hBrokerDll) FreeLibrary(g_hBrokerDll);
    g_hBrokerDll = nullptr;
    CleanupD3D();
}
int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow) {
    UNREFERENCED_PARAMETER(hPrevInstance); UNREFERENCED_PARAMETER(lpCmdLine);
    if (!IsRunningAsAdmin()) {
        MessageBox(NULL, L"You must launch this application as an Administrator to register the virtual camera.", L"Administrator Rights Required", MB_OK | MB_ICONERROR);
        return 1;
    }
    LoadStringW(hInstance, IDS_APP_TITLE, _title, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_VIRTUACAM, _windowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    if (FAILED(MFStartup(MF_VERSION))) { CoUninitialize(); return 1; }
    g_hMainWnd = InitInstance(hInstance, nCmdShow);
    if (!g_hMainWnd) { 
        MFShutdown(); 
        CoUninitialize();
        return FALSE; 
    }
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            if (g_pfnRenderBrokerFrame) g_pfnRenderBrokerFrame();
            if (g_hPreviewWnd && IsWindow(g_hPreviewWnd)) {
                RenderPreviewFrame(g_hPreviewWnd);
                UpdateTelemetry();
            } else {
                Sleep(10);
            }
        }
    }
    MFShutdown();
    CoUninitialize();
    return (int)msg.wParam;
}
ATOM MyRegisterClass(HINSTANCE instance) {
    WNDCLASSEXW wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = instance;
    wcex.lpszClassName = _windowClass;
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
HWND InitInstance(HINSTANCE instance, int cmd) {
    _instance = instance;
    HWND hwnd = CreateWindowW(_windowClass, _title, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, instance, nullptr);
    return hwnd;
}
LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        if (FAILED(LaunchAndLoadBroker())) {
             MessageBox(hwnd, L"Failed to load DirectPortBroker.dll.", L"Error", MB_OK | MB_ICONERROR);
             DestroyWindow(hwnd);
             break;
        }
        if (FAILED(RegisterVirtualCamera())) {
            MessageBox(hwnd, L"Failed to register and start the virtual camera.", L"Error", MB_OK | MB_ICONERROR);
            AddTrayIcon(hwnd, true);
        } else {
            AddTrayIcon(hwnd, true);
        }
        break;
    case WM_APP_TRAY_MSG:
        switch (lParam) {
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowContextMenu(hwnd);
            break;
        case WM_LBUTTONDBLCLK:
            CreatePreviewWindow();
            break;
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_TRAY_PREVIEW_WINDOW:
            CreatePreviewWindow();
            break;
        case ID_TRAY_ABOUT:
            DialogBox(_instance, MAKEINTRESOURCE(IDD_ABOUTBOX), g_hPreviewWnd ? g_hPreviewWnd : hwnd, About);
            break;
        case ID_TRAY_EXIT:
            DestroyWindow(hwnd);
            break;
        }
        break;
    case WM_DESTROY:
        AddTrayIcon(hwnd, false);
        ShutdownSystem();
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, message, wParam, lParam);
    }
    return 0;
}
LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        if (FAILED(InitD3D(hwnd)) || FAILED(LoadAssets())) {
            MessageBox(hwnd, L"Failed to initialize D3D for preview.", L"Error", MB_OK | MB_ICONERROR);
            return -1;
        }
        {
           g_hTelemetryLabel = CreateWindowW(L"STATIC", L"Status: Initializing...",
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                0, 0, 640, 20,
                hwnd, (HMENU)IDC_TELEMETRY_LABEL, _instance, NULL);
           ComPtr<ID3D11Texture2D> sharedTex;
           sharedTex.Attach(g_pfnGetSharedTexture());
           if (!sharedTex || FAILED(g_device->CreateShaderResourceView(sharedTex.Get(), nullptr, &g_previewSRV))) {
               MessageBox(hwnd, L"Failed to get shared texture from broker.", L"Error", MB_OK | MB_ICONERROR);
               return -1;
           }
        }
        break;
    case WM_SIZE:
        if (g_swapChain) {
            UINT width = LOWORD(lParam);
            UINT height = HIWORD(lParam);
            if (width == 0 || height == 0) break;
            g_context->OMSetRenderTargets(0, 0, 0); g_rtv.Reset();
            g_swapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
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
    UNREFERENCED_PARAMETER(lParam);
    switch (message) {
    case WM_INITDIALOG:
        CenterWindow(hDlg, true);
        return (INT_PTR)TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
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
        nid.hIcon = (HICON)LoadImage(_instance, MAKEINTRESOURCE(IDI_VIRTUACAM), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        wcscpy_s(nid.szTip, L"VirtuaCam");
        if (_vcam) {
             wcscat_s(nid.szTip, L" - Active");
        } else {
             wcscat_s(nid.szTip, L" - Inactive/Error");
        }
        Shell_NotifyIcon(NIM_ADD, &nid);
    } else {
        Shell_NotifyIcon(NIM_DELETE, &nid);
    }
}
void ShowContextMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    if (hMenu) {
        InsertMenu(hMenu, -1, MF_BYPOSITION, ID_TRAY_PREVIEW_WINDOW, L"Show Preview");
        InsertMenu(hMenu, -1, MF_BYPOSITION, ID_TRAY_ABOUT, L"About");
        InsertMenu(hMenu, -1, MF_SEPARATOR, 0, NULL);
        InsertMenu(hMenu, -1, MF_BYPOSITION, ID_TRAY_EXIT, L"Exit");
        SetForegroundWindow(hwnd);
        TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
        PostMessage(hwnd, WM_NULL, 0, 0);
        DestroyMenu(hMenu);
    }
}
void CreatePreviewWindow() {
    if (g_hPreviewWnd && IsWindow(g_hPreviewWnd)) {
        ShowWindow(g_hPreviewWnd, SW_SHOW);
        SetForegroundWindow(g_hPreviewWnd);
        return;
    }
    RECT rc = { 0, 0, 640, 360 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW & ~WS_SYSMENU, FALSE);
    g_hPreviewWnd = CreateWindowW(PREVIEW_WINDOW_CLASS, L"VirtuaCam Preview",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, _instance, NULL);
    if (g_hPreviewWnd) {
        CenterWindow(g_hPreviewWnd, true);
        ShowWindow(g_hPreviewWnd, SW_SHOW);
        UpdateWindow(g_hPreviewWnd);
    }
}
void UpdateTelemetry() {
    if (!g_hTelemetryLabel || !g_pfnGetBrokerState) return;
    BrokerState currentState = g_pfnGetBrokerState();
    if (currentState != g_lastBrokerState) {
        g_lastBrokerState = currentState;
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
bool IsRunningAsAdmin() {
    BOOL fIsAdmin = FALSE;
    HANDLE hToken = NULL;
    TOKEN_ELEVATION elevation;
    DWORD dwSize;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &dwSize)) {
            fIsAdmin = (elevation.TokenIsElevated != 0);
        }
        CloseHandle(hToken);
    }
    return fIsAdmin;
}
void RenderPreviewFrame(HWND hwnd) {
    if (!g_rtv || !g_previewSRV) return;
    const float clearColor[] = { 0.1f, 0.1f, 0.1f, 1.0f };
    g_context->ClearRenderTargetView(g_rtv.Get(), clearColor);
    RECT rc; GetClientRect(hwnd, &rc);
    float telemetryHeight = 20.0f;
    D3D11_VIEWPORT vp = { 0, telemetryHeight, (float)rc.right, (float)rc.bottom - telemetryHeight, 0, 1 };
    if (vp.Width <= 0 || vp.Height <= 0) return;
    g_context->RSSetViewports(1, &vp);
    g_context->OMSetRenderTargets(1, g_rtv.GetAddressOf(), nullptr);
    g_context->VSSetShader(g_vs.Get(), nullptr, 0);
    g_context->PSSetShader(g_ps.Get(), nullptr, 0);
    g_context->PSSetShaderResources(0, 1, g_previewSRV.GetAddressOf());
    g_context->PSSetSamplers(0, 1, g_sampler.GetAddressOf());
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->Draw(3, 0); 
    g_swapChain->Present(1, 0);
}
HRESULT InitD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &scd, &g_swapChain, &g_device, nullptr, &g_context);
    if (SUCCEEDED(hr)) {
        ComPtr<ID3D11Texture2D> pBuffer;
        g_swapChain->GetBuffer(0, IID_PPV_ARGS(&pBuffer));
        g_device->CreateRenderTargetView(pBuffer.Get(), NULL, &g_rtv);
    }
    return hr;
}
void CleanupD3D() {
    if(g_context) g_context->ClearState();
    g_rtv.Reset(); 
    g_swapChain.Reset(); 
    g_context.Reset(); 
    g_device.Reset();
    g_vs.Reset(); 
    g_ps.Reset(); 
    g_sampler.Reset(); 
    g_previewSRV.Reset();
}