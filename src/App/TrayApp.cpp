// =============================================================================
// TrayApp.cpp
// =============================================================================

#include "TrayApp.h"
#include "CameraIds.h"
#include "Discovery.h"
#include "Formats.h"
#include "Menu.h"

#include <dwmapi.h>
#include <mfapi.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>

namespace {

constexpr wchar_t kWindowClass[] = L"VirtuaCamTray";
constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT WM_APP_BROKER = WM_APP + 3;
constexpr UINT WM_APP_PRODUCER_EXITED = WM_APP + 4;
constexpr UINT kTrayIconId = 1;
constexpr UINT kFirstCommand = 100;

struct NamedItem { std::wstring name; std::wstring argument; };

std::vector<NamedItem> EnumerateCameras()
{
    std::vector<NamedItem> cameras;
    wil::com_ptr_nothrow<IMFAttributes> filter;
    if (FAILED(MFCreateAttributes(&filter, 1)) ||
        FAILED(filter->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)))
        return cameras;
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(filter.get(), &devices, &count)))
        return cameras;
    for (UINT32 i = 0; i < count; i++) {
        wil::unique_cotaskmem_string name, link;
        UINT32 length = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &length)) &&
            SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &length)) &&
            wcscmp(name.get(), kVirtuaCamFriendlyName) != 0)   // never feed the camera into itself
            cameras.push_back({ name.get(), link.get() });
        devices[i]->Release();
    }
    CoTaskMemFree(devices);
    return cameras;
}

std::vector<NamedItem> EnumerateDisplays()
{
    std::vector<NamedItem> displays;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info)) {
            auto* list = reinterpret_cast<std::vector<NamedItem>*>(data);
            std::wstring label = L"Display " + std::to_wstring(list->size() + 1) + L"  (" +
                std::to_wstring(info.rcMonitor.right - info.rcMonitor.left) + L"×" +
                std::to_wstring(info.rcMonitor.bottom - info.rcMonitor.top) +
                ((info.dwFlags & MONITORINFOF_PRIMARY) ? L", primary)" : L")");
            list->push_back({ label, info.szDevice });
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&displays));
    return displays;
}

std::vector<NamedItem> EnumerateWindows()
{
    std::vector<NamedItem> windows;
    EnumWindows([](HWND hwnd, LPARAM data) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindowTextLengthW(hwnd) == 0 ||
            (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
            return TRUE;
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked)
            return TRUE;
        wchar_t title[256];
        GetWindowTextW(hwnd, title, ARRAYSIZE(title));
        reinterpret_cast<std::vector<NamedItem>*>(data)->push_back(
            { title, std::to_wstring(reinterpret_cast<ULONG_PTR>(hwnd)) });
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

std::wstring Ellipsize(const std::wstring& text, size_t max = 40)
{
    return text.size() <= max ? text : text.substr(0, max - 1) + L"…";
}

} // namespace

// -----------------------------------------------------------------------------
// Startup and shutdown
// -----------------------------------------------------------------------------

int TrayApp::Run(HINSTANCE instance)
{
    m_instance = instance;
    m_settings = Settings::Load();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
    // A hidden top-level window (not message-only) so it receives the
    // TaskbarCreated broadcast after an Explorer restart.
    CreateWindowExW(0, kWindowClass, L"VirtuaCam", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);
    if (!m_hwnd)
        return 1;
    m_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    if (FAILED(m_producers.Initialize(m_hwnd, WM_APP_PRODUCER_EXITED)) ||
        FAILED(m_broker.Start(m_settings.outputWidth, m_settings.outputHeight, m_settings.outputFps, m_hwnd, WM_APP_BROKER))) {
        MessageBoxW(nullptr, L"VirtuaCam could not initialise Direct3D 11 on this machine.", L"VirtuaCam", MB_ICONERROR);
        return 1;
    }
    PopupMenu::SetPreviewRenderer([this](Presenter& presenter, const RECT& bounds) {
        m_broker.WithOutput([&](ID3D11ShaderResourceView* view, UINT width, UINT height) {
            presenter.Present(view, width, height, bounds, 6.0f);
        });
    });

    AddTrayIcon();
    const HRESULT camera = StartVirtualCamera();
    if (FAILED(camera)) {
        wchar_t code[16];
        swprintf_s(code, L"0x%08X", (unsigned)camera);
        Notify(L"Virtual camera unavailable",
               std::wstring(L"The camera could not be created (") + code +
               L"). VirtuaCam needs Windows 11 and its camera component registered by the installer.");
    }
    UpdateStatus();

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    m_preview.Close();
    m_producers.StopAll();
    m_broker.Stop();
    if (m_camera) {
        m_camera->Remove();
        m_camera.reset();
    }
    return 0;
}

HRESULT TrayApp::StartVirtualCamera()
{
    auto create = [this]() -> HRESULT {
        m_camera.reset();
        RETURN_IF_FAILED(MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session,
            MFVirtualCameraAccess_CurrentUser, kVirtuaCamFriendlyName, kVirtuaCamSourceClsidString, nullptr, 0, &m_camera));
        return m_camera->Start(nullptr);
    };

    const std::wstring classKey = std::wstring(L"Software\\Classes\\CLSID\\") + kVirtuaCamSourceClsidString + L"\\InprocServer32";
    const bool registered = RegGetValueW(HKEY_LOCAL_MACHINE, classKey.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    if (!registered &&
        MessageBoxW(nullptr, L"VirtuaCam's camera component is not registered on this PC yet.\n\n"
                             L"Register it now? Windows will ask for administrator approval once.",
                    L"VirtuaCam", MB_YESNO | MB_ICONQUESTION) == IDYES)
        RegisterCameraComponent();
    return create();
}

bool TrayApp::RegisterCameraComponent()
{
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    std::wstring dll(exe);
    dll = dll.substr(0, dll.find_last_of(L'\\') + 1) + kVirtuaCamSourceDll;
    const std::wstring parameters = L"/s \"" + dll + L"\"";

    SHELLEXECUTEINFOW execute{ sizeof(execute) };
    execute.fMask = SEE_MASK_NOCLOSEPROCESS;
    execute.lpVerb = L"runas";
    execute.lpFile = L"regsvr32.exe";
    execute.lpParameters = parameters.c_str();
    execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute) || !execute.hProcess)
        return false;
    wil::unique_handle process(execute.hProcess);
    WaitForSingleObject(process.get(), 30000);
    DWORD code = 1;
    GetExitCodeProcess(process.get(), &code);
    return code == 0;
}

void TrayApp::AddTrayIcon()
{
    NOTIFYICONDATAW icon{ sizeof(icon) };
    icon.hWnd = m_hwnd;
    icon.uID = kTrayIconId;
    icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    icon.uCallbackMessage = WM_APP_TRAY;
    LoadIconMetric(m_instance, MAKEINTRESOURCEW(1), LIM_SMALL, &icon.hIcon);
    wcscpy_s(icon.szTip, L"VirtuaCam");
    Shell_NotifyIconW(NIM_ADD, &icon);
    icon.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &icon);
    if (icon.hIcon)
        DestroyIcon(icon.hIcon);
}

void TrayApp::Notify(const std::wstring& title, const std::wstring& text)
{
    NOTIFYICONDATAW icon{ sizeof(icon) };
    icon.hWnd = m_hwnd;
    icon.uID = kTrayIconId;
    icon.uFlags = NIF_INFO;
    icon.dwInfoFlags = NIIF_WARNING;
    wcsncpy_s(icon.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(icon.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &icon);
}

void TrayApp::UpdateStatus()
{
    const BrokerStatus s = m_broker.Status();
    std::wstring status;
    if (!m_camera)
        status = L"camera unavailable";
    else if (s.wanted == 0)
        status = L"no source selected";
    else if (s.live == 0)
        status = L"waiting for source";
    else
        status = std::to_wstring(s.live) + (s.live == 1 ? L" source live" : L" sources live");
    status += L"  ·  " + std::to_wstring(s.width) + L"×" + std::to_wstring(s.height) + L" @ " + std::to_wstring(s.fps);
    if (status == m_status)
        return;
    m_status = status;

    NOTIFYICONDATAW icon{ sizeof(icon) };
    icon.hWnd = m_hwnd;
    icon.uID = kTrayIconId;
    icon.uFlags = NIF_TIP | NIF_SHOWTIP;
    wcsncpy_s(icon.szTip, (L"VirtuaCam — " + status).c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &icon);
    m_preview.SetStatus(status);
}

// -----------------------------------------------------------------------------
// Frames
// -----------------------------------------------------------------------------

void TrayApp::UpdateFrameNotifications()
{
    m_broker.SetFrameNotifications(m_preview.IsOpen() || PopupMenu::IsOpen());
}

void TrayApp::OnFrame()
{
    m_broker.FrameConsumed();
    UpdateStatus();
    PopupMenu::RefreshPreview();
    m_preview.Refresh();
    UpdateFrameNotifications();
}

// -----------------------------------------------------------------------------
// Sources
// -----------------------------------------------------------------------------

void TrayApp::Assign(int slot, Choice choice)
{
    if (m_grid && slot == MainSlot)
        m_grid = false;
    if (m_slots[slot] == choice) {
        PushLayout();
        return;
    }
    // Start the new producer before releasing the old one, so re-selecting a
    // camera that another slot also uses never closes and reopens it.
    DWORD pid = 0;
    if (choice.kind == Choice::Kind::Producer) {
        if (FAILED(m_producers.Acquire(choice.producer, &pid))) {
            Notify(L"Could not start source", choice.label);
            choice = Choice{};
        }
    } else if (choice.kind == Choice::Kind::External) {
        pid = choice.externalPid;
    }
    if (m_slots[slot].kind == Choice::Kind::Producer)
        m_producers.Release(m_slots[slot].producer);
    m_slots[slot] = choice;
    m_slotPids[slot] = pid;
    PushLayout();
}

void TrayApp::SetGrid(bool grid)
{
    m_grid = grid;
    PushLayout();
}

void TrayApp::PushLayout()
{
    BrokerLayout layout;
    layout.mode = m_grid ? LayoutMode::Grid : LayoutMode::Single;
    layout.main = m_slotPids[MainSlot];
    for (int i = 0; i < 4; i++)
        layout.pip[i] = m_slotPids[TopLeft + i];
    m_broker.SetLayout(layout);
}

void TrayApp::OnProducerExited(DWORD pid)
{
    ProducerSpec spec;
    DWORD exitCode = 0;
    if (!m_producers.OnExited(pid, &spec, &exitCode))
        return;   // stopped on purpose
    std::wstring label;
    for (int slot = 0; slot < SlotCount; slot++) {
        if (m_slots[slot].kind == Choice::Kind::Producer && m_slots[slot].producer == spec) {
            label = m_slots[slot].label;
            m_slots[slot] = Choice{};
            m_slotPids[slot] = 0;
        }
    }
    PushLayout();
    if (exitCode != 0 && spec.kind != L"whiteboard")
        Notify(L"Source stopped", label.empty() ? spec.kind : label);
}

// -----------------------------------------------------------------------------
// Menu
// -----------------------------------------------------------------------------

UINT TrayApp::Command(std::function<void()> action)
{
    m_commands.push_back(std::move(action));
    return kFirstCommand + (UINT)m_commands.size() - 1;
}

void TrayApp::AddSourceItems(PopupMenu* menu, int slot)
{
    const Choice& current = m_slots[slot];
    auto producerItem = [&](PopupMenu* target, const std::wstring& label, const std::wstring& kind, const std::wstring& argument) {
        Choice choice;
        choice.kind = Choice::Kind::Producer;
        choice.producer = { kind, argument };
        choice.label = label;
        const bool checked = !(slot == MainSlot && m_grid) && current == choice;
        target->AddItem(Ellipsize(label), Command([this, slot, choice] { Assign(slot, choice); }), checked);
    };

    menu->AddItem(L"Off", Command([this, slot] { Assign(slot, Choice{}); }),
                  current.kind == Choice::Kind::Off && !(slot == MainSlot && m_grid));
    if (slot == MainSlot)
        menu->AddItem(L"All sources (grid)", Command([this] { SetGrid(!m_grid); }), m_grid);
    menu->AddSeparator();

    const auto cameras = EnumerateCameras();
    for (const auto& camera : cameras)
        producerItem(menu, camera.name, L"camera", camera.argument);
    if (cameras.empty())
        menu->AddItem(L"No cameras found", 0, false, false);
    menu->AddSeparator();

    for (const auto& display : EnumerateDisplays())
        producerItem(menu, display.name, L"display", display.argument);
    PopupMenu* windows = menu->AddSubMenu(L"Window");
    for (const auto& window : EnumerateWindows())
        producerItem(windows, window.name, L"window", window.argument);
    menu->AddSeparator();
    producerItem(menu, L"Whiteboard", L"whiteboard", L"");

    // Producers started by other applications (anything speaking the
    // shared-frame protocol), excluding our own producer processes.
    bool header = false;
    for (const auto& producer : DiscoverProducers(m_broker.Adapter())) {
        if (m_producers.Owns(producer.pid))
            continue;
        if (!header) {
            menu->AddSeparator();
            header = true;
        }
        Choice choice;
        choice.kind = Choice::Kind::External;
        choice.externalPid = producer.pid;
        choice.label = producer.processName + L"  (" + std::to_wstring(producer.width) + L"×" + std::to_wstring(producer.height) + L")";
        menu->AddItem(Ellipsize(choice.label), Command([this, slot, choice] { Assign(slot, choice); }), current == choice);
    }
}

void TrayApp::ShowMenu(POINT anchor)
{
    m_commands.clear();
    auto* menu = new PopupMenu(m_hwnd, m_instance);
    menu->AddPreview(Command([this] { m_preview.Open(m_instance, &m_broker); UpdateFrameNotifications(); }));
    menu->AddItem(L"Open preview", Command([this] { m_preview.Open(m_instance, &m_broker); UpdateFrameNotifications(); }));
    menu->AddSeparator();

    AddSourceItems(menu->AddSubMenu(L"Source"), MainSlot);
    PopupMenu* pip = menu->AddSubMenu(L"Picture in picture");
    static const wchar_t* const corners[] = { L"Top left", L"Top right", L"Bottom left", L"Bottom right" };
    for (int i = 0; i < 4; i++) {
        const Choice& choice = m_slots[TopLeft + i];
        std::wstring title = corners[i];
        if (choice.kind != Choice::Kind::Off)
            title += L"  ·  " + Ellipsize(choice.label, 24);
        AddSourceItems(pip->AddSubMenu(title), TopLeft + i);
    }
    menu->AddSeparator();

    PopupMenu* output = menu->AddSubMenu(L"Output");
    for (const auto& size : Formats::kOutputSizes) {
        const bool checked = size.width == m_settings.outputWidth && size.height == m_settings.outputHeight;
        output->AddItem(std::to_wstring(size.width) + L" × " + std::to_wstring(size.height), Command([this, size] {
            m_settings.outputWidth = size.width;
            m_settings.outputHeight = size.height;
            m_settings.Save();
            m_broker.SetOutput(m_settings.outputWidth, m_settings.outputHeight, m_settings.outputFps);
        }), checked);
    }
    output->AddSeparator();
    for (UINT fps : Formats::kOutputRates) {
        output->AddItem(std::to_wstring(fps) + L" fps", Command([this, fps] {
            m_settings.outputFps = fps;
            m_settings.Save();
            m_broker.SetOutput(m_settings.outputWidth, m_settings.outputHeight, m_settings.outputFps);
        }), fps == m_settings.outputFps);
    }
    menu->AddItem(L"Start with Windows", Command([] { SetAutostartEnabled(!IsAutostartEnabled()); }), IsAutostartEnabled());
    menu->AddSeparator();
    menu->AddItem(L"About VirtuaCam", Command([this] {
        MessageBoxW(m_hwnd,
            L"VirtuaCam\n\nA virtual camera that composites GPU-shared frames from any producer: "
            L"cameras, windows, displays, the whiteboard, or your own application.\n\nMIT License.",
            L"About VirtuaCam", MB_ICONINFORMATION);
    }));
    menu->AddItem(L"Exit", Command([this] { DestroyWindow(m_hwnd); }));

    menu->ShowAt(anchor);
    UpdateFrameNotifications();
}

// -----------------------------------------------------------------------------
// Window procedure
// -----------------------------------------------------------------------------

LRESULT CALLBACK TrayApp::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        auto* self = static_cast<TrayApp*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    }
    auto* self = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT TrayApp::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == m_taskbarCreated && m_taskbarCreated) {
        AddTrayIcon();
        m_status.clear();
        UpdateStatus();
        return 0;
    }
    switch (message) {
    case WM_APP_TRAY:
        switch (LOWORD(lParam)) {
        case WM_CONTEXTMENU:
        case NIN_SELECT:
        case NIN_KEYSELECT:
            ShowMenu({ GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam) });
            break;
        case WM_LBUTTONDBLCLK:
            PopupMenu::CloseAll();
            m_preview.Open(m_instance, &m_broker);
            UpdateFrameNotifications();
            break;
        }
        return 0;
    case WM_APP_MENU_COMMAND: {
        const UINT index = (UINT)wParam - kFirstCommand;
        if (wParam >= kFirstCommand && index < m_commands.size()) {
            auto action = m_commands[index];   // copy: the action may rebuild m_commands
            action();
        }
        UpdateFrameNotifications();
        return 0;
    }
    case WM_APP_BROKER:
        OnFrame();
        return 0;
    case WM_APP_PRODUCER_EXITED:
        OnProducerExited((DWORD)wParam);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
            PopupMenu::CloseAll();
        return 0;
    case WM_DESTROY: {
        PopupMenu::CloseAll();
        NOTIFYICONDATAW icon{ sizeof(icon) };
        icon.hWnd = m_hwnd;
        icon.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &icon);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(m_hwnd, message, wParam, lParam);
}
