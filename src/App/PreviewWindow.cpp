// =============================================================================
// PreviewWindow.cpp
// =============================================================================
// A Mica Alt window; the video is a DirectComposition visual inset from the
// edges with rounded corners, so the backdrop frames it.  Nothing is painted
// with GDI and nothing renders unless the broker published a frame.
// =============================================================================

#include "PreviewWindow.h"
#include "Broker.h"

#include <dwmapi.h>

namespace {
constexpr wchar_t kClass[] = L"VirtuaCamPreview";
}

void PreviewWindow::Open(HINSTANCE instance, Broker* broker)
{
    if (m_hwnd) {
        ShowWindow(m_hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(m_hwnd);
        return;
    }
    m_broker = broker;

    static const bool registered = [instance] {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        wc.lpszClassName = kClass;
        return RegisterClassExW(&wc) != 0;
    }();
    (void)registered;

    const UINT dpi = GetDpiForSystem();
    RECT frame{ 0, 0, MulDiv(800, dpi, 96), MulDiv(474, dpi, 96) };
    AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    CreateWindowExW(0, kClass, L"VirtuaCam Preview", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                    frame.right - frame.left, frame.bottom - frame.top, nullptr, nullptr, instance, this);
    if (!m_hwnd)
        return;

    const BOOL dark = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_TABBEDWINDOW;   // Mica Alt
    DwmSetWindowAttribute(m_hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    const MARGINS margins{ -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(m_hwnd, &margins);

    m_presenter.Attach(m_hwnd, false);
    ShowWindow(m_hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(m_hwnd);
    Refresh();
}

void PreviewWindow::Close()
{
    if (m_hwnd)
        DestroyWindow(m_hwnd);
}

void PreviewWindow::SetStatus(const std::wstring& status)
{
    if (m_hwnd)
        SetWindowTextW(m_hwnd, (L"VirtuaCam Preview  —  " + status).c_str());
}

void PreviewWindow::Refresh()
{
    if (!m_hwnd || !m_broker || IsIconic(m_hwnd))
        return;
    RECT client;
    GetClientRect(m_hwnd, &client);
    const int inset = MulDiv(12, GetDpiForWindow(m_hwnd), 96);
    InflateRect(&client, -inset, -inset);
    if (client.right <= client.left || client.bottom <= client.top)
        return;
    m_broker->WithOutput([&](ID3D11ShaderResourceView* view, UINT width, UINT height) {
        m_presenter.Present(view, width, height, client, (float)inset * 0.75f);
    });
}

LRESULT CALLBACK PreviewWindow::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        auto* self = static_cast<PreviewWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    }
    auto* self = reinterpret_cast<PreviewWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT PreviewWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
            Refresh();
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
            DestroyWindow(m_hwnd);
        return 0;
    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(m_hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DESTROY:
        m_presenter.Detach();
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
        m_hwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(m_hwnd, message, wParam, lParam);
}
