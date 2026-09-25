// =============================================================================
// Menu.cpp  --  Custom-drawn tray menu
// =============================================================================
// Look: the window extends its frame over the whole client area and asks DWM
// for the Mica Alt backdrop (DWMSBT_TABBEDWINDOW), dark mode and rounded
// corners.  Painting goes through a 32bpp buffered-paint DIB so per-pixel
// alpha reaches DWM; text therefore uses DrawThemeTextEx(DTT_COMPOSITED),
// since plain GDI text would come out fully transparent.
//
// Input: the root menu holds mouse capture while open, so every click --
// inside any open submenu or anywhere else on screen -- arrives here and is
// routed by screen position.  Losing capture (Alt+Tab, another app grabbing
// the mouse) closes the whole chain.
//
// Thumbnail: a Presenter (DirectComposition) sits above the preview item and
// is refreshed only when the broker publishes a new frame.
// =============================================================================

#include "Menu.h"
#include "Presenter.h"

#include <dwmapi.h>
#include <shellscalingapi.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <algorithm>

namespace {

constexpr wchar_t kMenuClass[] = L"VirtuaCamPopupMenu";
constexpr int kItemHeight = 32;
constexpr int kSeparatorHeight = 9;
constexpr int kPreviewWidth = 288;

std::vector<PopupMenu*> g_open;          // open menus, root first
PopupMenu::PreviewRenderer g_renderer;

int Scale(int value, int dpi) { return MulDiv(value, dpi, 96); }

HFONT MenuFont(int dpi)
{
    static int cachedDpi = 0;
    static HFONT cached = nullptr;
    if (cached && cachedDpi == dpi)
        return cached;
    NONCLIENTMETRICSW metrics{ sizeof(metrics) };
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi))
        return (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (cached)
        DeleteObject(cached);
    cached = CreateFontIndirectW(&metrics.lfMenuFont);
    cachedDpi = dpi;
    return cached;
}

// Alpha-blends a solid colour over a 32bpp surface (premultiplied).
void FillAlpha(HDC hdc, const RECT& rc, COLORREF color, BYTE alpha)
{
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 1;
    info.bmiHeader.biHeight = 1;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC source = CreateCompatibleDC(hdc);
    HBITMAP pixel = CreateDIBSection(source, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (pixel && bits) {
        *static_cast<UINT32*>(bits) = ((UINT32)alpha << 24) |
            ((UINT32)(GetRValue(color) * alpha / 255) << 16) |
            ((UINT32)(GetGValue(color) * alpha / 255) << 8) |
            (UINT32)(GetBValue(color) * alpha / 255);
        HGDIOBJ previous = SelectObject(source, pixel);
        const BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        AlphaBlend(hdc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, source, 0, 0, 1, 1, blend);
        SelectObject(source, previous);
    }
    if (pixel)
        DeleteObject(pixel);
    DeleteDC(source);
}

void DrawLabel(HTHEME theme, HDC hdc, const std::wstring& text, RECT rc, DWORD format, COLORREF color)
{
    DTTOPTS options{ sizeof(options) };
    options.dwFlags = DTT_COMPOSITED | DTT_TEXTCOLOR;
    options.crText = color;
    DrawThemeTextEx(theme, hdc, 0, 0, text.c_str(), -1, format | DT_NOPREFIX | DT_SINGLELINE | DT_VCENTER, &rc, &options);
}

int DpiAt(POINT screen)
{
    UINT dpiX = 96, dpiY = 96;
    if (FAILED(GetDpiForMonitor(MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
        return 96;
    return (int)dpiX;
}

} // namespace

// -----------------------------------------------------------------------------
// Construction
// -----------------------------------------------------------------------------

PopupMenu::PopupMenu(HWND owner, HINSTANCE instance) : m_owner(owner), m_instance(instance)
{
    static const bool registered = [instance] {
        BufferedPaintInit();
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kMenuClass;
        return RegisterClassExW(&wc) != 0;
    }();
    (void)registered;
}

PopupMenu::~PopupMenu() = default;

void PopupMenu::AddItem(const std::wstring& text, UINT id, bool checked, bool enabled)
{
    Item item;
    item.text = text;
    item.id = id;
    item.checked = checked;
    item.enabled = enabled;
    m_items.push_back(std::move(item));
}

void PopupMenu::AddSeparator()
{
    if (m_items.empty() || m_items.back().separator)
        return;
    Item item;
    item.separator = true;
    m_items.push_back(std::move(item));
}

void PopupMenu::AddPreview(UINT id)
{
    Item item;
    item.id = id;
    item.preview = true;
    m_items.push_back(std::move(item));
}

PopupMenu* PopupMenu::AddSubMenu(const std::wstring& text)
{
    Item item;
    item.text = text;
    item.subMenu = std::make_unique<PopupMenu>(m_owner, m_instance);
    item.subMenu->m_parent = this;
    PopupMenu* sub = item.subMenu.get();
    m_items.push_back(std::move(item));
    return sub;
}

void PopupMenu::SetPreviewRenderer(PreviewRenderer renderer) { g_renderer = std::move(renderer); }
bool PopupMenu::IsOpen() { return !g_open.empty(); }

void PopupMenu::CloseAll()
{
    if (!g_open.empty() && IsWindow(g_open.front()->m_hwnd))
        DestroyWindow(g_open.front()->m_hwnd);
}

void PopupMenu::RefreshPreview()
{
    for (PopupMenu* menu : g_open)
        menu->UpdatePreview();
}

// -----------------------------------------------------------------------------
// Geometry
// -----------------------------------------------------------------------------

int PopupMenu::ItemHeight(const Item& item) const
{
    if (item.separator)
        return Scale(kSeparatorHeight, m_dpi);
    if (item.preview)
        return (Width() - Scale(16, m_dpi)) * 9 / 16 + Scale(12, m_dpi);
    return Scale(kItemHeight, m_dpi);
}

int PopupMenu::Width() const
{
    if (m_width)
        return m_width;
    HDC hdc = GetDC(nullptr);
    HGDIOBJ previous = SelectObject(hdc, MenuFont(m_dpi));
    int widest = 0;
    bool hasPreview = false;
    for (const auto& item : m_items) {
        hasPreview |= item.preview;
        SIZE size{};
        if (!item.separator && !item.preview && GetTextExtentPoint32W(hdc, item.text.c_str(), (int)item.text.size(), &size))
            widest = std::max(widest, (int)size.cx);
    }
    SelectObject(hdc, previous);
    ReleaseDC(nullptr, hdc);
    m_width = widest + Scale(84, m_dpi);
    if (hasPreview)
        m_width = std::max(m_width, Scale(kPreviewWidth, m_dpi));
    return m_width;
}

int PopupMenu::Height() const
{
    int height = Scale(4, m_dpi) * 2;
    for (const auto& item : m_items)
        height += ItemHeight(item);
    return height;
}

RECT PopupMenu::ItemRect(size_t index) const
{
    int top = Scale(4, m_dpi);
    for (size_t i = 0; i < index; i++)
        top += ItemHeight(m_items[i]);
    return { 0, top, Width(), top + ItemHeight(m_items[index]) };
}

int PopupMenu::HitTest(POINT client) const
{
    for (size_t i = 0; i < m_items.size(); i++) {
        const RECT rc = ItemRect(i);
        if (PtInRect(&rc, client))
            return m_items[i].separator ? -1 : (int)i;
    }
    return -1;
}

// -----------------------------------------------------------------------------
// Showing and submenus
// -----------------------------------------------------------------------------

void PopupMenu::ShowAt(POINT anchor)
{
    CloseAll();
    m_dpi = DpiAt(anchor);
    MONITORINFO monitor{ sizeof(monitor) };
    GetMonitorInfoW(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &monitor);
    int x = anchor.x, y = anchor.y;
    if (x + Width() > monitor.rcWork.right) x = anchor.x - Width();
    if (y + Height() > monitor.rcWork.bottom) y = anchor.y - Height();
    // Tray menus must own the foreground so a click elsewhere dismisses them.
    SetForegroundWindow(m_owner);
    Show(x, y);
}

void PopupMenu::Show(int x, int y)
{
    m_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kMenuClass, L"", WS_POPUP,
                             x, y, Width(), Height(), m_parent ? m_parent->m_hwnd : m_owner, nullptr, m_instance, this);
    if (!m_hwnd) {
        if (!m_parent)
            delete this;
        return;
    }

    const MARGINS margins{ -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(m_hwnd, &margins);
    const DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_TABBEDWINDOW;   // Mica Alt
    DwmSetWindowAttribute(m_hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));

    g_open.push_back(this);
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    if (!m_parent)
        SetCapture(m_hwnd);
    UpdatePreview();
}

void PopupMenu::OpenSubMenu(int index)
{
    CloseSubMenu();
    PopupMenu* sub = m_items[index].subMenu.get();
    if (!sub || sub->m_items.empty())
        return;
    sub->m_dpi = m_dpi;
    RECT item = ItemRect(index);
    MapWindowPoints(m_hwnd, nullptr, reinterpret_cast<POINT*>(&item), 2);
    MONITORINFO monitor{ sizeof(monitor) };
    GetMonitorInfoW(MonitorFromRect(&item, MONITOR_DEFAULTTONEAREST), &monitor);
    int x = item.right - Scale(4, m_dpi);
    if (x + sub->Width() > monitor.rcWork.right)
        x = item.left - sub->Width() + Scale(4, m_dpi);
    int y = item.top - Scale(4, m_dpi);
    if (y + sub->Height() > monitor.rcWork.bottom)
        y = std::max<int>(monitor.rcWork.top, monitor.rcWork.bottom - sub->Height());
    m_openSub = sub;
    sub->Show(x, y);
}

void PopupMenu::CloseSubMenu()
{
    if (m_openSub && IsWindow(m_openSub->m_hwnd))
        DestroyWindow(m_openSub->m_hwnd);
    m_openSub = nullptr;
}

void PopupMenu::Hover(int index)
{
    if (index == m_hover)
        return;
    m_hover = index;
    InvalidateRect(m_hwnd, nullptr, FALSE);
    if (index >= 0 && m_items[index].subMenu) {
        if (m_openSub != m_items[index].subMenu.get())
            OpenSubMenu(index);
    } else {
        CloseSubMenu();
    }
}

void PopupMenu::UpdatePreview()
{
    for (size_t i = 0; i < m_items.size(); i++) {
        if (!m_items[i].preview)
            continue;
        if (!m_presenter) {
            m_presenter = std::make_unique<Presenter>();
            m_presenter->Attach(m_hwnd, true);
        }
        RECT box = ItemRect(i);
        InflateRect(&box, -Scale(8, m_dpi), -Scale(6, m_dpi));
        if (g_renderer)
            g_renderer(*m_presenter, box);
    }
}

// -----------------------------------------------------------------------------
// Painting
// -----------------------------------------------------------------------------

void PopupMenu::Paint(HDC hdc)
{
    RECT client;
    GetClientRect(m_hwnd, &client);
    // Faint tint keeps text legible on bright wallpapers.
    FillAlpha(hdc, client, RGB(18, 18, 22), 110);

    HTHEME theme = OpenThemeData(m_hwnd, L"CompositedWindow::Window");
    HGDIOBJ previousFont = SelectObject(hdc, MenuFont(m_dpi));
    const int pad = Scale(4, m_dpi);

    for (size_t i = 0; i < m_items.size(); i++) {
        const Item& item = m_items[i];
        RECT rc = ItemRect(i);
        if (item.separator) {
            RECT line{ rc.left + Scale(12, m_dpi), (rc.top + rc.bottom) / 2, rc.right - Scale(12, m_dpi), (rc.top + rc.bottom) / 2 + 1 };
            FillAlpha(hdc, line, RGB(255, 255, 255), 36);
            continue;
        }
        if (item.preview) {
            RECT plate = rc;
            InflateRect(&plate, -Scale(8, m_dpi), -Scale(6, m_dpi));
            FillAlpha(hdc, plate, RGB(0, 0, 0), 120);   // letterbox behind the video visual
            continue;
        }
        if ((int)i == m_hover && item.enabled) {
            RECT highlight = rc;
            InflateRect(&highlight, -pad, -Scale(2, m_dpi));
            FillAlpha(hdc, highlight, RGB(255, 255, 255), 26);
        }
        const COLORREF color = item.enabled ? RGB(240, 240, 240) : RGB(130, 130, 136);
        RECT text = rc;
        text.left += Scale(36, m_dpi);
        text.right -= Scale(28, m_dpi);
        DrawLabel(theme, hdc, item.text, text, DT_END_ELLIPSIS, color);
        if (item.checked) {
            RECT check{ rc.left + Scale(8, m_dpi), rc.top, rc.left + Scale(30, m_dpi), rc.bottom };
            DrawLabel(theme, hdc, L"✓", check, DT_CENTER, color);
        }
        if (item.subMenu) {
            RECT arrow{ rc.right - Scale(28, m_dpi), rc.top, rc.right - Scale(10, m_dpi), rc.bottom };
            DrawLabel(theme, hdc, L"›", arrow, DT_CENTER, RGB(200, 200, 200));
        }
    }

    SelectObject(hdc, previousFont);
    if (theme)
        CloseThemeData(theme);
}

// -----------------------------------------------------------------------------
// Window procedure
// -----------------------------------------------------------------------------

LRESULT CALLBACK PopupMenu::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        auto* self = static_cast<PopupMenu*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    }
    auto* self = reinterpret_cast<PopupMenu*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT PopupMenu::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    // Screen point -> (deepest open menu under it, client point).
    auto route = [](POINT screen, POINT& client) -> PopupMenu* {
        for (auto it = g_open.rbegin(); it != g_open.rend(); ++it) {
            RECT rc;
            GetWindowRect((*it)->m_hwnd, &rc);
            if (PtInRect(&rc, screen)) {
                client = screen;
                ScreenToClient((*it)->m_hwnd, &client);
                return *it;
            }
        }
        return nullptr;
    };

    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(m_hwnd, &ps);
        RECT client;
        GetClientRect(m_hwnd, &client);
        HDC buffer = nullptr;
        HPAINTBUFFER paint = BeginBufferedPaint(hdc, &client, BPBF_TOPDOWNDIB, nullptr, &buffer);
        if (paint) {
            BufferedPaintClear(paint, nullptr);
            Paint(buffer);
            EndBufferedPaint(paint, TRUE);
        }
        EndPaint(m_hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEMOVE: {
        if (m_parent)
            break;   // only the root (capture holder) routes input
        POINT screen{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(m_hwnd, &screen);
        POINT client{};
        if (PopupMenu* target = route(screen, client))
            target->Hover(target->HitTest(client));
        return 0;
    }
    case WM_LBUTTONUP: {
        if (m_parent)
            break;
        POINT screen{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(m_hwnd, &screen);
        POINT client{};
        PopupMenu* target = route(screen, client);
        if (target) {
            const int index = target->HitTest(client);
            if (index < 0 || target->m_items[index].subMenu || !target->m_items[index].enabled)
                return 0;   // separators, submenu headers, disabled items keep the menu open
            const UINT id = target->m_items[index].id;
            const HWND owner = m_owner;
            CloseAll();
            PostMessageW(owner, WM_APP_MENU_COMMAND, id, 0);
            return 0;
        }
        CloseAll();
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        if (m_parent)
            break;
        POINT screen{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(m_hwnd, &screen);
        POINT client{};
        if (!route(screen, client))
            CloseAll();   // click outside every menu
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
            CloseAll();
        return 0;
    case WM_CAPTURECHANGED:
        if (!m_parent && reinterpret_cast<HWND>(lParam) != m_hwnd)
            CloseAll();
        return 0;

    case WM_DESTROY: {
        CloseSubMenu();
        if (m_parent && m_parent->m_openSub == this)
            m_parent->m_openSub = nullptr;
        if (m_presenter)
            m_presenter->Detach();
        g_open.erase(std::remove(g_open.begin(), g_open.end(), this), g_open.end());
        if (!m_parent && GetCapture() == m_hwnd)
            ReleaseCapture();
        m_hover = -1;
        return 0;
    }
    case WM_NCDESTROY: {
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
        m_hwnd = nullptr;
        // The root owns itself; submenus belong to their parent item.
        if (!m_parent)
            delete this;
        return 0;
    }
    }
    return DefWindowProcW(m_hwnd, message, wParam, lParam);
}
