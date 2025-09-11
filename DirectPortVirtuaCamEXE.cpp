#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <string>
#include <shellapi.h>

#include <wil/com.h>
#include <wil/result.h>
#include <wil/resource.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

DEFINE_GUID(CLSID_DirectPortVirtualCamera, 0x08675309, 0x4a4e, 0x4e59, 0x86, 0x75, 0x30, 0x9a, 0x44, 0x9b, 0x00, 0x00);
DEFINE_GUID(IID_IDirectPortVirtuaCamControl, 0x90675309, 0x4a4e, 0x4e59, 0x86, 0x75, 0x30, 0x9a, 0x44, 0x9b, 0x00, 0x01);
DEFINE_GUID(MR_CAMERA_CONTROL_SERVICE, 0x90675309, 0x4a4e, 0x4e59, 0x86, 0x75, 0x30, 0x9a, 0x44, 0x9b, 0x00, 0x02);

const WCHAR VCAM_FRIENDLY_NAME[] = L"DirectPort VirtuaCam";
const WCHAR MANAGER_WND_CLASS[] = L"DirectPortVirtuaCamManagerWnd_90675309";
const UINT IDI_VirtuaCam = 101;

#define WM_APP_TRAY_MSG (WM_APP + 1)
#define ID_TRAY_MENU_TOGGLE 1001
#define ID_TRAY_MENU_PREVIEW 1002
#define ID_TRAY_MENU_EXIT 1003

interface IDirectPortVirtuaCamControl : public IUnknown
{
    STDMETHOD(TogglePreviewWindow)() = 0;
    STDMETHOD(IsPreviewWindowVisible)(BOOL* pIsVisible) = 0;
};

struct AppState
{
    HINSTANCE hInstance = nullptr;
    HWND hManagerWnd = nullptr;
    HMENU hMenu = nullptr;
    bool isCameraEnabled = false;
    wil::com_ptr_nothrow<IMFVirtualCamera> virtualCamera;
};

LRESULT CALLBACK ManagerWndProc(HWND, UINT, WPARAM, LPARAM);
void ShowContextMenu(HWND hwnd);
void ToggleVirtualCamera(AppState* pState);
void TogglePreviewWindow(AppState* pState);
void UpdateMenuState(AppState* pState);

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = ManagerWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = MANAGER_WND_CLASS;
    RegisterClassW(&wc);
    
    AppState state;
    state.hInstance = hInstance;
    state.hManagerWnd = CreateWindowW(MANAGER_WND_CLASS, VCAM_FRIENDLY_NAME, 0, 0, 0, 0, 0, nullptr, nullptr, hInstance, &state);
    if (!state.hManagerWnd) return 1;

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}

LRESULT CALLBACK ManagerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    AppState* pState = reinterpret_cast<AppState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (msg)
    {
        case WM_CREATE:
        {
            CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
            pState = reinterpret_cast<AppState*>(pCreate->lpCreateParams);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)pState);
            
            NOTIFYICONDATAW nid = { sizeof(nid) };
            nid.hWnd = hwnd;
            nid.uID = 1;
            nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            nid.uCallbackMessage = WM_APP_TRAY_MSG;
            nid.hIcon = (HICON)LoadImage(pState->hInstance, MAKEINTRESOURCE(IDI_VirtuaCam), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
            wcscpy_s(nid.szTip, ARRAYSIZE(nid.szTip), VCAM_FRIENDLY_NAME);
            Shell_NotifyIconW(NIM_ADD, &nid);

            pState->hMenu = CreatePopupMenu();
            AppendMenuW(pState->hMenu, MF_STRING, ID_TRAY_MENU_TOGGLE, L"Enable Virtual Camera");
            AppendMenuW(pState->hMenu, MF_STRING | MF_GRAYED, ID_TRAY_MENU_PREVIEW, L"Show Preview");
            AppendMenuW(pState->hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(pState->hMenu, MF_STRING, ID_TRAY_MENU_EXIT, L"Exit");

            return 0;
        }
        case WM_APP_TRAY_MSG:
        {
            if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_LBUTTONUP)
            {
                ShowContextMenu(hwnd);
            }
            return 0;
        }
        case WM_COMMAND:
        {
            switch (LOWORD(wParam))
            {
            case ID_TRAY_MENU_TOGGLE:
                ToggleVirtualCamera(pState);
                break;
            case ID_TRAY_MENU_PREVIEW:
                TogglePreviewWindow(pState);
                break;
            case ID_TRAY_MENU_EXIT:
                DestroyWindow(hwnd);
                break;
            }
            return 0;
        }
        case WM_DESTROY:
        {
            if (pState->isCameraEnabled)
            {
                ToggleVirtualCamera(pState);
            }
            NOTIFYICONDATAW nid = { sizeof(nid) };
            nid.hWnd = hwnd;
            nid.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            DestroyMenu(pState->hMenu);
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void ShowContextMenu(HWND hwnd)
{
    AppState* pState = reinterpret_cast<AppState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    UpdateMenuState(pState);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(pState->hMenu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, NULL);
    PostMessage(hwnd, WM_NULL, 0, 0);
}

void ToggleVirtualCamera(AppState* pState)
{
    HRESULT hr = S_OK;
    if (pState->isCameraEnabled)
    {
        if (pState->virtualCamera)
        {
            pState->virtualCamera->Shutdown();
            pState->virtualCamera->Remove();
            pState->virtualCamera.reset();
        }
        pState->isCameraEnabled = false;
        MFShutdown();
        CoUninitialize();
    }
    else
    {
        hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr))
        {
            hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
            if (SUCCEEDED(hr))
            {
                wchar_t clsidString[40];
                StringFromGUID2(CLSID_DirectPortVirtualCamera, clsidString, ARRAYSIZE(clsidString));
                hr = MFCreateVirtualCamera(
                    MFVirtualCameraType_SoftwareCameraSource,
                    MFVirtualCameraLifetime_Session,
                    MFVirtualCameraAccess_CurrentUser,
                    VCAM_FRIENDLY_NAME,
                    clsidString,
                    nullptr, 0,
                    &pState->virtualCamera);
                
                if (SUCCEEDED(hr))
                {
                    hr = pState->virtualCamera->Start(nullptr);
                }
            }
        }
        
        if (SUCCEEDED(hr))
        {
            pState->isCameraEnabled = true;
        }
        else
        {
            MessageBoxW(pState->hManagerWnd, L"Failed to create and start the virtual camera.", L"Error", MB_ICONERROR);
            if (pState->virtualCamera) pState->virtualCamera.reset();
            MFShutdown();
            CoUninitialize();
        }
    }
    UpdateMenuState(pState);
}

void TogglePreviewWindow(AppState* pState)
{
    if (!pState->isCameraEnabled || !pState->virtualCamera) return;

    wil::com_ptr_nothrow<IDirectPortVirtuaCamControl> control;
    if (SUCCEEDED(pState->virtualCamera->GetService(MR_CAMERA_CONTROL_SERVICE, IID_PPV_ARGS(&control))))
    {
        control->TogglePreviewWindow();
    }
    UpdateMenuState(pState);
}

void UpdateMenuState(AppState* pState)
{
    if (pState->isCameraEnabled)
    {
        ModifyMenuW(pState->hMenu, ID_TRAY_MENU_TOGGLE, MF_BYCOMMAND | MF_CHECKED, ID_TRAY_MENU_TOGGLE, L"Disable Virtual Camera");
        EnableMenuItem(pState->hMenu, ID_TRAY_MENU_PREVIEW, MF_BYCOMMAND | MF_ENABLED);
        
        wil::com_ptr_nothrow<IDirectPortVirtuaCamControl> control;
        if (SUCCEEDED(pState->virtualCamera->GetService(MR_CAMERA_CONTROL_SERVICE, IID_PPV_ARGS(&control))))
        {
            BOOL isVisible = FALSE;
            if (SUCCEEDED(control->IsPreviewWindowVisible(&isVisible)) && isVisible)
            {
                ModifyMenuW(pState->hMenu, ID_TRAY_MENU_PREVIEW, MF_BYCOMMAND | MF_STRING, ID_TRAY_MENU_PREVIEW, L"Hide Preview");
            }
            else
            {
                ModifyMenuW(pState->hMenu, ID_TRAY_MENU_PREVIEW, MF_BYCOMMAND | MF_STRING, ID_TRAY_MENU_PREVIEW, L"Show Preview");
            }
        }
    }
    else
    {
        ModifyMenuW(pState->hMenu, ID_TRAY_MENU_TOGGLE, MF_BYCOMMAND | MF_UNCHECKED, ID_TRAY_MENU_TOGGLE, L"Enable Virtual Camera");
        EnableMenuItem(pState->hMenu, ID_TRAY_MENU_PREVIEW, MF_BYCOMMAND | MF_GRAYED);
        ModifyMenuW(pState->hMenu, ID_TRAY_MENU_PREVIEW, MF_BYCOMMAND | MF_STRING, ID_TRAY_MENU_PREVIEW, L"Show Preview");
    }
}