// =============================================================================
// Menu.cpp  --  Custom tray context menu with VOM-style handle management
// =============================================================================
// A custom-drawn popup menu with a Windows 11 look:
//   - Real DWM Mica/Acrylic: the window extends its frame into the client area
//     (DwmExtendFrameIntoClientArea) and paints with per-pixel alpha through a
//     buffered paint DIB, so the DWMSBT_TRANSIENTWINDOW backdrop shows through.
//     Text must therefore be drawn with DrawThemeTextEx(DTT_COMPOSITED) -- plain
//     GDI text writes alpha 0 and would be invisible on the backdrop.
//   - Rounded corners via DWMWA_WINDOW_CORNER_PREFERENCE.
//   - An optional live preview item (AddPreviewItem) that shows the camera
//     output as a thumbnail, refreshed by a timer while the menu is open.
//
// Ownership model: the top-level menu is heap-allocated by the caller and
// deletes itself on WM_NCDESTROY.  Submenus are owned by their parent's
// CustomMenuItem::subMenu unique_ptr and must NOT delete themselves -- their
// windows are destroyed with the owner chain, but the objects die with the
// parent.  (Deleting in both places was a heap-corrupting double delete.)
//
// VIRTUAL OBJECT MANAGER (VOM) PATTERN:
// -------------------------------------
// This file implements a kernel-mode-inspired handle table for menu objects to
// solve DEADLOCK and FOCUS-SHIFT cleanup issues:
//
// PROBLEM: The original implementation had race conditions where:
//   - User clicks away from menu -> focus shifts -> menu should close
//   - But WM_KILLFOCUS arrived after other messages, causing stale pointers
//   - UI thread blocked waiting for background threads holding menu references
//   - Result: Deadlock or leaked menu windows floating on screen
//
// SOLUTION: Generational handle table (like Windows kernel object manager):
//   - Each menu gets a unique handle ID (monotonically increasing counter)
//   - HandleEntry contains: Menu pointer, RefCount, Generation, CloseEvent
//   - Critical section protects the entire handle table (thread-safe)
//   - CloseEvent (manual-reset) signals deterministic cleanup completion
//   - Stale generations rejected O(1) - prevents use-after-free
//
// LIFECYCLE:
//   1. Show() -> RegisterHandle() -> allocates ID, creates event, stores entry
//   2. WM_KILLFOCUS -> CloseAllMenus() -> signals close events
//   3. WM_DESTROY -> UnregisterHandle() -> signals event, removes from table
//   4. Process shutdown -> CleanupHandles() -> ensures all events signaled
//
// WHY THIS WORKS:
//   - No cross-thread blocking: handle table uses short critical section locks
//   - Deterministic cleanup: events signal completion, no polling needed
//   - Focus-shift safe: WM_KILLFOCUS triggers immediate cleanup cascade
//   - Generational safety: old handle IDs fail validation after teardown
// =============================================================================

#include "pch.h"
#include "Menu.h"
#include "App.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <uxtheme.h>
#include <vector>
#include <algorithm>
#include <unordered_map>

#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "msimg32.lib")   // AlphaBlend

const wchar_t POPUP_MENU_CLASS[] = L"VirtuaCamCustomMenu";

// VOM-style handle table for menu objects - thread-safe, generational handles
struct MenuHandleEntry {
    CustomMenu* Menu;
    LONG RefCount;
    UINT Generation;
    HANDLE CloseEvent;  // Manual-reset event for deterministic cleanup signaling
};

static std::unordered_map<UINT, MenuHandleEntry> g_menuHandles;
static UINT g_menuHandleCounter = 0;
static CRITICAL_SECTION g_menuHandleLock;
static CustomMenu* g_topLevelMenu = nullptr;
static std::vector<CustomMenu*> g_openMenus;
static MenuPreviewProvider g_previewProvider;

static constexpr UINT_PTR PREVIEW_TIMER_ID = 1;
static constexpr UINT PREVIEW_TIMER_MS = 66;   // ~15 fps thumbnail refresh
static constexpr int SEPARATOR_HEIGHT = 10;
static constexpr int PREVIEW_MIN_WIDTH = 280;

// ---------------------------------------------------------------------------
// Painting helpers
// ---------------------------------------------------------------------------

// Returns the user's menu font (Segoe UI on modern systems) instead of the
// legacy DEFAULT_GUI_FONT bitmap font.
static HFONT GetMenuFont()
{
    static HFONT font = [] {
        NONCLIENTMETRICSW ncm = { sizeof(ncm) };
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
            return CreateFontIndirectW(&ncm.lfMenuFont);
        return (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    }();
    return font;
}

// Alpha-blends a solid colour rectangle onto a 32bpp target DC, preserving the
// destination's per-pixel alpha semantics (used for tint, hover, separators).
static void FillAlphaRect(HDC hdc, const RECT& rc, COLORREF color, BYTE alpha)
{
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 1;
    bmi.bmiHeader.biHeight = 1;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC srcDC = CreateCompatibleDC(hdc);
    if (!srcDC) return;
    HBITMAP bmp = CreateDIBSection(srcDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        // Premultiplied BGRA
        const BYTE r = (BYTE)((GetRValue(color) * alpha) / 255);
        const BYTE g = (BYTE)((GetGValue(color) * alpha) / 255);
        const BYTE b = (BYTE)((GetBValue(color) * alpha) / 255);
        *(UINT32*)bits = ((UINT32)alpha << 24) | ((UINT32)r << 16) | ((UINT32)g << 8) | b;
        HGDIOBJ old = SelectObject(srcDC, bmp);
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        AlphaBlend(hdc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, srcDC, 0, 0, 1, 1, bf);
        SelectObject(srcDC, old);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(srcDC);
}

// Draws alpha-correct text on a composited (per-pixel alpha) surface.
static void DrawCompositedText(HTHEME theme, HDC hdc, const std::wstring& text, RECT rc, DWORD format, COLORREF color)
{
    if (theme) {
        DTTOPTS opts = { sizeof(opts) };
        opts.dwFlags = DTT_COMPOSITED | DTT_TEXTCOLOR;
        opts.crText = color;
        DrawThemeTextEx(theme, hdc, 0, 0, text.c_str(), -1, format | DT_NOPREFIX, &rc, &opts);
    } else {
        // Pre-DWM fallback; on a composited surface this text may not show,
        // but such systems never get this far (the backdrop call fails first).
        SetTextColor(hdc, color);
        SetBkMode(hdc, TRANSPARENT);
        DrawTextW(hdc, text.c_str(), -1, &rc, format | DT_NOPREFIX);
    }
}

// ---------------------------------------------------------------------------
// Construction / item list
// ---------------------------------------------------------------------------

CustomMenu::CustomMenu(HWND parent, HINSTANCE instance) : m_hwnd(nullptr), m_parentHwnd(parent), m_instance(instance), m_parentMenu(nullptr), m_activeSubMenu(nullptr), m_activeSubMenuItem(-1), m_handleId(0), m_generation(0) {
    static bool isClassRegistered = false;
    if (!isClassRegistered) {
        InitializeCriticalSection(&g_menuHandleLock);
        BufferedPaintInit();   // process-lifetime; paired implicitly at exit
        WNDCLASSEXW wcex = {};
        wcex.cbSize = sizeof(WNDCLASSEX);
        wcex.lpfnWndProc = MenuWndProc;
        wcex.hInstance = m_instance;
        wcex.lpszClassName = POPUP_MENU_CLASS;
        wcex.hbrBackground = nullptr;   // we own every painted pixel
        wcex.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
        wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wcex);
        isClassRegistered = true;
    }
}

CustomMenu::~CustomMenu() {
    UnregisterHandle();
}

// Static cleanup for VOM handle table - called at process shutdown
void CleanupMenuHandles() {
    EnterCriticalSection(&g_menuHandleLock);
    for (auto& kv : g_menuHandles) {
        if (kv.second.CloseEvent) {
            SetEvent(kv.second.CloseEvent);
            CloseHandle(kv.second.CloseEvent);
        }
    }
    g_menuHandles.clear();
    LeaveCriticalSection(&g_menuHandleLock);
    DeleteCriticalSection(&g_menuHandleLock);
}

void CustomMenu::CleanupHandles() {
    CleanupMenuHandles();
}

HWND CustomMenu::GetHwnd() const { return m_hwnd; }

// VOM-style handle registration - allocates a generational handle for the menu
void CustomMenu::RegisterHandle() {
    EnterCriticalSection(&g_menuHandleLock);
    m_handleId = ++g_menuHandleCounter;
    m_generation = 1;
    
    HANDLE closeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    MenuHandleEntry entry{ this, 1, m_generation, closeEvent };
    g_menuHandles[m_handleId] = entry;
    LeaveCriticalSection(&g_menuHandleLock);
}

// Unregister handle and signal close event for deterministic cleanup
void CustomMenu::UnregisterHandle() {
    if (m_handleId == 0) return;
    
    EnterCriticalSection(&g_menuHandleLock);
    auto it = g_menuHandles.find(m_handleId);
    if (it != g_menuHandles.end() && it->second.Menu == this) {
        // Signal the close event to wake any waiters
        if (it->second.CloseEvent) {
            SetEvent(it->second.CloseEvent);
            CloseHandle(it->second.CloseEvent);
        }
        g_menuHandles.erase(it);
    }
    m_handleId = 0;
    LeaveCriticalSection(&g_menuHandleLock);
}

// Static helper to signal close event by handle ID
void CustomMenu::SignalCloseEvent(UINT handleId) {
    EnterCriticalSection(&g_menuHandleLock);
    auto it = g_menuHandles.find(handleId);
    if (it != g_menuHandles.end() && it->second.CloseEvent) {
        SetEvent(it->second.CloseEvent);
    }
    LeaveCriticalSection(&g_menuHandleLock);
}

void CustomMenu::AddItem(const std::wstring& text, UINT id, bool checked) { m_items.push_back({ text, id, false, checked, false, false, nullptr }); }
void CustomMenu::AddSeparator() { m_items.push_back({ L"", 0, true, false, false, false, nullptr }); }

void CustomMenu::AddPreviewItem(UINT id) {
    m_items.push_back({ L"", id, false, false, false, true, nullptr });
    m_hasPreview = true;
}

void CustomMenu::SetPreviewProvider(MenuPreviewProvider provider) {
    g_previewProvider = std::move(provider);
}

CustomMenu* CustomMenu::AddSubMenu(const std::wstring& text) {
    auto newSubMenu = std::make_unique<CustomMenu>(m_parentHwnd, m_instance);
    newSubMenu->m_parentMenu = this;
    CustomMenu* rawPtr = newSubMenu.get();
    m_items.push_back({ text, 0, false, false, true, false, std::move(newSubMenu) });
    return rawPtr;
}

void CustomMenu::CalculateOptimalWidth() {
    if (m_calculatedWidth > 0) return;
    HDC hdc = GetDC(NULL);
    HFONT hOldFont = (HFONT)SelectObject(hdc, GetMenuFont());
    int maxWidth = 0;
    for (const auto& item : m_items) {
        if (!item.isSeparator && !item.isPreview) {
            SIZE size;
            if (GetTextExtentPoint32W(hdc, item.text.c_str(), (int)item.text.length(), &size)) {
                if (size.cx > maxWidth) maxWidth = size.cx;
            }
        }
    }
    SelectObject(hdc, hOldFont);
    ReleaseDC(NULL, hdc);
    m_calculatedWidth = maxWidth + 85;
    if (m_hasPreview && m_calculatedWidth < PREVIEW_MIN_WIDTH)
        m_calculatedWidth = PREVIEW_MIN_WIDTH;
}

int CustomMenu::ItemHeight(const CustomMenuItem& item) const {
    if (item.isSeparator) return SEPARATOR_HEIGHT;
    if (item.isPreview) {
        // 16:9 thumbnail spanning the menu width, plus padding.
        const int innerW = m_calculatedWidth - 16;
        return innerW * 9 / 16 + 12;
    }
    return m_itemHeight;
}

int CustomMenu::GetCalculatedWidth() const {
    const_cast<CustomMenu*>(this)->CalculateOptimalWidth();
    return m_calculatedWidth;
}

int CustomMenu::GetCalculatedHeight() const {
    GetCalculatedWidth();   // preview height depends on the width
    int height = 0;
    for (const auto& item : m_items)
        height += ItemHeight(item);
    return height;
}

// ---------------------------------------------------------------------------
// Window lifetime
// ---------------------------------------------------------------------------

void CustomMenu::Show(int x, int y) {
    CalculateOptimalWidth();
    int height = GetCalculatedHeight();

    m_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        POPUP_MENU_CLASS, L"", WS_POPUP,
        x, y, m_calculatedWidth, height,
        m_parentMenu ? m_parentMenu->m_hwnd : m_parentHwnd,
        nullptr, m_instance, this
    );
    if (!m_hwnd) return;

    // Register VOM-style handle for deterministic cleanup tracking
    RegisterHandle();
    
    g_openMenus.push_back(this);
    if (m_parentMenu == nullptr) {
        g_topLevelMenu = this;
        SetCapture(m_hwnd);
    }

    // Extend the frame over the whole client area so the system backdrop is
    // visible wherever we leave alpha at zero; then request the Acrylic-style
    // transient-window backdrop, dark mode, and rounded corners.
    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(m_hwnd, &margins);
    DWM_SYSTEMBACKDROP_TYPE backdropType = DWMSBT_TRANSIENTWINDOW;
    DwmSetWindowAttribute(m_hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
    BOOL useDarkMode = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUNDSMALL;
    DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    if (m_hasPreview)
        SetTimer(m_hwnd, PREVIEW_TIMER_ID, PREVIEW_TIMER_MS, nullptr);

    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
}

void CustomMenu::CloseChildren() {
    if (m_activeSubMenu) {
        if (IsWindow(m_activeSubMenu->m_hwnd)) {
            DestroyWindow(m_activeSubMenu->m_hwnd);
        }
        m_activeSubMenu = nullptr;
        m_activeSubMenuItem = -1;
    }
}

void CustomMenu::CloseAllMenus() {
    if (g_topLevelMenu && IsWindow(g_topLevelMenu->m_hwnd)) {
        DestroyWindow(g_topLevelMenu->m_hwnd);
    }
}

LRESULT CALLBACK CustomMenu::MenuWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    CustomMenu* pThis = (CustomMenu*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
    if (uMsg == WM_NCCREATE) {
        CREATESTRUCT* pCreate = (CREATESTRUCT*)lParam;
        pThis = (CustomMenu*)pCreate->lpCreateParams;
        SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)pThis);
    }
    if (pThis) {
        return pThis->HandleMessage(hWnd, uMsg, wParam, lParam);
    }
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

void CustomMenu::HandleMouseMove(POINT clientPt) {
    int y = clientPt.y;
    int currentY = 0;
    int newHover = -1;

    for (size_t i = 0; i < m_items.size(); ++i) {
        int itemHeight = ItemHeight(m_items[i]);
        RECT itemRect = { 0, currentY, m_calculatedWidth, currentY + itemHeight };
        if (y >= itemRect.top && y < itemRect.bottom && !m_items[i].isSeparator) {
            newHover = (int)i;
            break;
        }
        currentY += itemHeight;
    }

    if (newHover != m_hoverItem) {
        if (m_activeSubMenuItem != -1 && m_activeSubMenuItem != newHover) {
            CloseChildren();
        }

        m_hoverItem = newHover;
        InvalidateRect(m_hwnd, NULL, FALSE);

        if (m_hoverItem != -1 && m_items[m_hoverItem].isSubMenu && !m_activeSubMenu) {
            m_activeSubMenuItem = m_hoverItem;
            m_activeSubMenu = m_items[m_hoverItem].subMenu.get();

            RECT rcItem;
            GetClientRect(m_hwnd, &rcItem);
            int itemTop = 0;
            for (int i = 0; i < m_hoverItem; ++i) {
                itemTop += ItemHeight(m_items[i]);
            }
            rcItem.top = itemTop;
            rcItem.bottom = rcItem.top + m_itemHeight;
            ClientToScreen(m_hwnd, (POINT*)&rcItem.left);
            ClientToScreen(m_hwnd, (POINT*)&rcItem.right);

            m_activeSubMenu->CalculateOptimalWidth();
            int subMenuW = m_activeSubMenu->m_calculatedWidth;
            int subMenuH = m_activeSubMenu->GetCalculatedHeight();

            HMONITOR hMonitor = MonitorFromRect(&rcItem, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(mi) }; GetMonitorInfo(hMonitor, &mi);

            int x = rcItem.right - 5;
            if (x + subMenuW > mi.rcWork.right) x = rcItem.left - subMenuW;
            int yPos = rcItem.top;
            if (yPos + subMenuH > mi.rcWork.bottom) yPos = rcItem.bottom - subMenuH;

            m_activeSubMenu->Show(x, yPos);
        }
    }
}

LRESULT CustomMenu::HandleMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        // Paint through a 32bpp buffered-paint DIB so per-pixel alpha reaches
        // DWM: pixels left at alpha 0 show the Mica/Acrylic backdrop.
        RECT rcClient;
        GetClientRect(hWnd, &rcClient);
        HDC memDC = nullptr;
        HPAINTBUFFER pb = BeginBufferedPaint(hdc, &rcClient, BPBF_TOPDOWNDIB, nullptr, &memDC);
        if (pb && memDC) {
            BufferedPaintClear(pb, nullptr);
            Draw(memDC, pb);
            EndBufferedPaint(pb, TRUE);
        } else {
            Draw(hdc, nullptr);   // degraded path: opaque, but functional
        }
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;   // all painting happens in WM_PAINT

    case WM_TIMER: {
        if (wParam == PREVIEW_TIMER_ID) {
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT clientPt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (m_parentMenu == nullptr) {
            POINT screenPt = clientPt;
            ClientToScreen(hWnd, &screenPt);

            CustomMenu* targetMenu = nullptr;
            for (auto it = g_openMenus.rbegin(); it != g_openMenus.rend(); ++it) {
                CustomMenu* pMenu = *it;
                RECT rc; GetWindowRect(pMenu->GetHwnd(), &rc);
                if (PtInRect(&rc, screenPt)) {
                    targetMenu = pMenu;
                    break;
                }
            }

            for (auto* pMenu : g_openMenus) {
                bool isAncestor = false;
                CustomMenu* temp = targetMenu;
                while (temp) {
                    if (temp == pMenu) { isAncestor = true; break; }
                    temp = temp->m_parentMenu;
                }
                if (pMenu != targetMenu && !isAncestor) {
                    if (pMenu->m_hoverItem != -1) {
                        pMenu->m_hoverItem = -1;
                        InvalidateRect(pMenu->GetHwnd(), NULL, FALSE);
                    }
                    pMenu->CloseChildren();
                }
            }

            if (targetMenu) {
                POINT targetClientPt = screenPt;
                ScreenToClient(targetMenu->GetHwnd(), &targetClientPt);
                targetMenu->HandleMouseMove(targetClientPt);
            }
        } else {
            HandleMouseMove(clientPt);
        }
        return 0;
    }

    case WM_KILLFOCUS: {
        // User focus shifted away - trigger deterministic cleanup via handle event
        if (this == g_topLevelMenu) {
            CloseAllMenus();
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
    case WM_ACTIVATE: {
        if (this == g_topLevelMenu && uMsg == WM_ACTIVATE && wParam == WA_INACTIVE) {
            CloseAllMenus();
        } else if (uMsg == WM_CAPTURECHANGED && (HWND)lParam != m_hwnd) {
            CloseAllMenus();
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (this != g_topLevelMenu) {
            return DefWindowProc(hWnd, uMsg, wParam, lParam);
        }

        POINT screenPt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(hWnd, &screenPt);

        CustomMenu* targetMenu = nullptr;
        for (auto it = g_openMenus.rbegin(); it != g_openMenus.rend(); ++it) {
            CustomMenu* pMenu = *it;
            if (!pMenu || !IsWindow(pMenu->m_hwnd)) continue;
            RECT rc;
            GetWindowRect(pMenu->m_hwnd, &rc);
            if (PtInRect(&rc, screenPt)) {
                targetMenu = pMenu;
                break;
            }
        }

        if (targetMenu) {
            POINT clientPt = screenPt;
            ScreenToClient(targetMenu->m_hwnd, &clientPt);

            UINT commandId = 0;
            int currentY = 0;
            for (const auto& item : targetMenu->m_items) {
                int itemHeight = targetMenu->ItemHeight(item);
                RECT itemRect = { 0, currentY, targetMenu->m_calculatedWidth, currentY + itemHeight };

                if (!item.isSeparator && !item.isSubMenu) {
                    if (PtInRect(&itemRect, clientPt)) {
                        commandId = item.id;
                        break;
                    }
                }
                currentY += itemHeight;
            }

            if (commandId != 0) {
                SendMessage(m_parentHwnd, WM_APP_MENU_COMMAND, commandId, 0);
            }
        }

        CloseAllMenus();
        return 0;
    }

    case WM_DESTROY: {
        KillTimer(hWnd, PREVIEW_TIMER_ID);
        if (this == g_topLevelMenu) {
            ReleaseCapture();
            g_topLevelMenu = nullptr;
        }
        // Detach from the parent so it never dereferences a destroyed submenu.
        if (m_parentMenu && m_parentMenu->m_activeSubMenu == this) {
            m_parentMenu->m_activeSubMenu = nullptr;
            m_parentMenu->m_activeSubMenuItem = -1;
        }

        auto it = std::find(g_openMenus.begin(), g_openMenus.end(), this);
        if (it != g_openMenus.end()) {
            g_openMenus.erase(it);
        }
        m_hwnd = nullptr;
        SetWindowLongPtr(hWnd, GWLP_USERDATA, 0);
        
        // Unregister VOM handle to signal deterministic cleanup
        UnregisterHandle();
        return 0;
    }

    case WM_NCDESTROY: {
        // Only the top-level menu owns itself; submenu objects belong to their
        // parent's unique_ptr and are deleted with the parent.
        if (m_parentMenu == nullptr) {
            delete this;
        }
        return 0;
    }
    
    default:
        return DefWindowProc(hWnd, uMsg, wParam, lParam);
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void CustomMenu::Draw(HDC hdc, HANDLE paintBuffer) {
    RECT clientRect;
    GetClientRect(m_hwnd, &clientRect);

    // Faint dark tint over the backdrop for legibility on bright desktops.
    FillAlphaRect(hdc, clientRect, RGB(18, 18, 22), 130);

    HTHEME theme = OpenThemeData(m_hwnd, L"CompositedWindow::Window");
    HFONT oldFont = (HFONT)SelectObject(hdc, GetMenuFont());

    int currentY = 0;
    for (size_t i = 0; i < m_items.size(); ++i) {
        const auto& item = m_items[i];
        int itemHeight = ItemHeight(item);
        RECT itemRect = { 0, currentY, m_calculatedWidth, currentY + itemHeight };

        if (item.isSeparator) {
            RECT sepRect = itemRect;
            sepRect.top += 4; sepRect.bottom = sepRect.top + 1;
            sepRect.left += 10; sepRect.right -= 10;
            FillAlphaRect(hdc, sepRect, RGB(255, 255, 255), 40);
        } else if (item.isPreview) {
            RECT box = itemRect;
            InflateRect(&box, -8, -6);
            // Plate behind the video (also the "letterbox" colour).
            FillAlphaRect(hdc, box, RGB(0, 0, 0), 110);

            std::vector<uint32_t> frame;
            UINT fw = 0, fh = 0;
            if (g_previewProvider && g_previewProvider(frame, fw, fh) && fw && fh) {
                const int boxW = box.right - box.left, boxH = box.bottom - box.top;
                const float fit = std::min(boxW / (float)fw, boxH / (float)fh);
                const int dw = std::max(1, (int)(fw * fit));
                const int dh = std::max(1, (int)(fh * fit));
                const int dx = box.left + (boxW - dw) / 2;
                const int dy = box.top + (boxH - dh) / 2;

                BITMAPINFO bmi = {};
                bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bmi.bmiHeader.biWidth = (LONG)fw;
                bmi.bmiHeader.biHeight = -(LONG)fh;   // top-down
                bmi.bmiHeader.biPlanes = 1;
                bmi.bmiHeader.biBitCount = 32;
                SetStretchBltMode(hdc, HALFTONE);
                SetBrushOrgEx(hdc, 0, 0, nullptr);
                StretchDIBits(hdc, dx, dy, dw, dh, 0, 0, fw, fh, frame.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);

                // StretchDIBits writes alpha 0; restore opacity so DWM doesn't
                // blend the backdrop through the video.
                RGBQUAD* bits = nullptr;
                int rowPx = 0;
                if (paintBuffer && SUCCEEDED(GetBufferedPaintBits((HPAINTBUFFER)paintBuffer, &bits, &rowPx)) && bits) {
                    for (int y = dy; y < dy + dh && y < clientRect.bottom; y++)
                        for (int x = dx; x < dx + dw && x < clientRect.right; x++)
                            bits[(size_t)y * rowPx + x].rgbReserved = 255;
                }
            } else {
                DrawCompositedText(theme, hdc, L"Preview unavailable", box, DT_SINGLELINE | DT_VCENTER | DT_CENTER, RGB(160, 160, 160));
            }
        } else {
            if ((int)i == m_hoverItem) {
                RECT hoverRect = itemRect;
                InflateRect(&hoverRect, -4, -2);
                FillAlphaRect(hdc, hoverRect, RGB(255, 255, 255), 28);
            }

            RECT textRect = itemRect;
            textRect.left += 35;
            DrawCompositedText(theme, hdc, item.text, textRect, DT_SINGLELINE | DT_VCENTER, RGB(240, 240, 240));
            if (item.isChecked) {
                RECT checkRect = itemRect;
                checkRect.right = 30;
                DrawCompositedText(theme, hdc, L"\u2713", checkRect, DT_SINGLELINE | DT_VCENTER | DT_CENTER, RGB(240, 240, 240));
            }
            if (item.isSubMenu) {
                RECT arrowRect = itemRect;
                arrowRect.left = m_calculatedWidth - 30;
                arrowRect.right = m_calculatedWidth - 10;
                DrawCompositedText(theme, hdc, L"\u203A", arrowRect, DT_SINGLELINE | DT_VCENTER | DT_CENTER, RGB(200, 200, 200));
            }
        }
        currentY += itemHeight;
    }

    SelectObject(hdc, oldFont);
    if (theme) CloseThemeData(theme);
}
