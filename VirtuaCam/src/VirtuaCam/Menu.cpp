#include "pch.h"
#include "Menu.h"
#include "App.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <vector>

const wchar_t POPUP_MENU_CLASS[] = L"VirtuaCamCustomMenu";

#define WM_APP_CLOSE_MENUS (WM_APP + 100)

static std::vector<CustomMenu*> g_openMenus;
static CustomMenu* g_topLevelMenu = nullptr;
static HHOOK g_mouseHook = nullptr;

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN) {
            MSLLHOOKSTRUCT* pMouseStruct = (MSLLHOOKSTRUCT*)lParam;
            if (pMouseStruct) {
                bool isClickInside = false;
                for (const auto& menu : g_openMenus) {
                    if (menu && menu->GetHwnd()) {
                        RECT menuRect;
                        if (GetWindowRect(menu->GetHwnd(), &menuRect)) {
                            if (PtInRect(&menuRect, pMouseStruct->pt)) {
                                isClickInside = true;
                                break;
                            }
                        }
                    }
                }

                if (!isClickInside) {
                    if (g_topLevelMenu && g_topLevelMenu->GetHwnd()) {
                        PostMessage(g_topLevelMenu->GetHwnd(), WM_APP_CLOSE_MENUS, 0, 0);
                    }
                }
            }
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}


CustomMenu::CustomMenu(HWND parent, HINSTANCE instance) : m_parentHwnd(parent), m_instance(instance), m_hwnd(nullptr) {
    static bool isClassRegistered = false;
    if (!isClassRegistered) {
        WNDCLASSEXW wcex = {};
        wcex.cbSize = sizeof(WNDCLASSEX);
        wcex.lpfnWndProc = MenuWndProc;
        wcex.hInstance = m_instance;
        wcex.lpszClassName = POPUP_MENU_CLASS;
        wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wcex.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
        wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wcex);
        isClassRegistered = true;
    }
}

CustomMenu::~CustomMenu() {
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
    }
}

HWND CustomMenu::GetHwnd() const {
    return m_hwnd;
}

void CustomMenu::AddItem(const std::wstring& text, UINT id, bool checked) {
    m_items.push_back({ text, id, false, checked, false, nullptr });
}

void CustomMenu::AddSeparator() {
    m_items.push_back({ L"", 0, true, false, false, nullptr });
}

CustomMenu* CustomMenu::AddSubMenu(const std::wstring& text) {
    auto newSubMenu = std::make_unique<CustomMenu>(m_parentHwnd, m_instance);
    newSubMenu->m_parentMenu = this;
    CustomMenu* rawPtr = newSubMenu.get();
    m_items.push_back({ text, 0, false, false, true, std::move(newSubMenu) });
    return rawPtr;
}

int CustomMenu::GetWidth() {
    if (m_calculatedWidth == 0) {
        CalculateOptimalWidth();
    }
    return m_calculatedWidth;
}

void CustomMenu::CalculateOptimalWidth() {
    HDC hdc = GetDC(NULL);
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

    int maxWidth = 0;
    for (const auto& item : m_items) {
        if (!item.isSeparator) {
            SIZE size;
            if (GetTextExtentPoint32W(hdc, item.text.c_str(), (int)item.text.length(), &size)) {
                if (size.cx > maxWidth) {
                    maxWidth = size.cx;
                }
            }
        }
    }

    SelectObject(hdc, hOldFont);
    ReleaseDC(NULL, hdc);

    m_calculatedWidth = maxWidth + 85;
}

int CustomMenu::CalculateHeight() const {
    int height = 0;
    for (const auto& item : m_items) {
        height += item.isSeparator ? 10 : m_itemHeight;
    }
    return height;
}

UINT CustomMenu::Show(int x, int y, std::function<void()> onIdle) {
    m_onIdle = onIdle;
    m_width = GetWidth();
    int height = CalculateHeight();

    m_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        POPUP_MENU_CLASS, L"", WS_POPUP,
        x, y, m_width, height,
        m_parentMenu ? m_parentMenu->m_hwnd : m_parentHwnd,
        nullptr, m_instance, this
    );

    if (!m_hwnd) return 0;

    DWM_SYSTEMBACKDROP_TYPE backdropType = DWMSBT_TRANSIENTWINDOW;
    DwmSetWindowAttribute(m_hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
    BOOL useDarkMode = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));

    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);

    if (m_parentMenu == nullptr) {
        g_topLevelMenu = this;
        g_openMenus.push_back(this);
        g_mouseHook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, m_instance, 0);
    } else {
        g_openMenus.push_back(this);
    }
    return 0;
}

void CustomMenu::CloseChildren() {
    if (m_activeSubMenu) {
        m_activeSubMenu->CloseChildren();
        DestroyWindow(m_activeSubMenu->m_hwnd);
        m_activeSubMenu = nullptr;
        m_activeSubMenuItem = -1;
    }
}

void CustomMenu::CloseAllMenus() {
    if (g_topLevelMenu) {
        DestroyWindow(g_topLevelMenu->m_hwnd);
    }
}

LRESULT CALLBACK CustomMenu::MenuWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    CustomMenu* pThis = nullptr;
    if (uMsg == WM_NCCREATE) {
        CREATESTRUCT* pCreate = (CREATESTRUCT*)lParam;
        pThis = (CustomMenu*)pCreate->lpCreateParams;
        SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)pThis);
    } else {
        pThis = (CustomMenu*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
    }
    if (pThis) {
        return pThis->HandleMessage(hWnd, uMsg, wParam, lParam);
    }
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

LRESULT CustomMenu::HandleMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        Draw(hdc);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_APP_CLOSE_MENUS:
        CloseAllMenus();
        return 0;
    case WM_KILLFOCUS: {
        HWND focusHwnd = (HWND)wParam;
        bool isChildMenu = false;
        for (const auto& menu : g_openMenus) {
            if (menu && menu->m_hwnd == focusHwnd) {
                isChildMenu = true;
                break;
            }
        }
        if (!isChildMenu) {
            CloseAllMenus();
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        int y = GET_Y_LPARAM(lParam);
        int currentY = 0;
        int newHover = -1;
        for (size_t i = 0; i < m_items.size(); ++i) {
            int itemHeight = m_items[i].isSeparator ? 10 : m_itemHeight;
            RECT itemRect = { 0, currentY, m_width, currentY + itemHeight };
            if (y >= itemRect.top && y < itemRect.bottom && !m_items[i].isSeparator) {
                newHover = (int)i;
                break;
            }
            currentY += itemHeight;
        }

        if (newHover != m_hoverItem) {
            if (m_activeSubMenuItem != newHover) {
                CloseChildren();
            }

            m_hoverItem = newHover;

            if (m_hoverItem != -1 && m_items[m_hoverItem].isSubMenu) {
                m_activeSubMenuItem = m_hoverItem;
                m_activeSubMenu = m_items[m_hoverItem].subMenu.get();

                RECT rcItem;
                GetClientRect(hWnd, &rcItem);
                
                int itemTop = 0;
                for(int i = 0; i < m_hoverItem; ++i) {
                    itemTop += m_items[i].isSeparator ? 10 : m_itemHeight;
                }
                rcItem.top = itemTop;
                rcItem.bottom = rcItem.top + m_itemHeight;

                ClientToScreen(hWnd, (POINT*)&rcItem.left);
                ClientToScreen(hWnd, (POINT*)&rcItem.right);

                int subMenuW = m_activeSubMenu->GetWidth();
                int subMenuH = m_activeSubMenu->CalculateHeight();
                HMONITOR hMonitor = MonitorFromRect(&rcItem, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = { sizeof(mi) };
                GetMonitorInfo(hMonitor, &mi);

                int x = rcItem.right - 5;
                if (x + subMenuW > mi.rcWork.right) {
                    x = rcItem.left - subMenuW;
                }

                int y = rcItem.top;
                if (y + subMenuH > mi.rcWork.bottom) {
                    y = rcItem.bottom - subMenuH;
                }
                
                m_activeSubMenu->Show(x, y, m_onIdle);
            }
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (m_hoverItem != -1) {
            const auto& item = m_items[m_hoverItem];
            if (!item.isSubMenu && item.id != 0) {
                PostMessage(m_parentHwnd, WM_APP_MENU_COMMAND, item.id, 0);
                CloseAllMenus();
            }
        }
        return 0;
    }
    case WM_DESTROY:
        std::erase(g_openMenus, this);
        if (this == g_topLevelMenu) {
            g_topLevelMenu = nullptr;
            if (g_mouseHook) {
                UnhookWindowsHookEx(g_mouseHook);
                g_mouseHook = nullptr;
            }
            delete this;
        }
        m_hwnd = nullptr;
        return 0;
    }
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

void CustomMenu::Draw(HDC hdc) {
    RECT clientRect;
    GetClientRect(m_hwnd, &clientRect);

    HBRUSH bgBrush = CreateSolidBrush(RGB(32, 32, 32));
    FillRect(hdc, &clientRect, bgBrush);
    DeleteObject(bgBrush);

    SetBkMode(hdc, TRANSPARENT);
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SelectObject(hdc, hFont);

    int currentY = 0;
    for (size_t i = 0; i < m_items.size(); ++i) {
        const auto& item = m_items[i];
        int itemHeight = item.isSeparator ? 10 : m_itemHeight;
        RECT itemRect = { 0, currentY, m_width, currentY + itemHeight };

        if (item.isSeparator) {
            RECT sepRect = itemRect;
            sepRect.top += 4; sepRect.bottom = sepRect.top + 1;
            sepRect.left += 10; sepRect.right -= 10;
            HBRUSH sepBrush = CreateSolidBrush(RGB(64, 64, 64));
            FillRect(hdc, &sepRect, sepBrush);
            DeleteObject(sepBrush);
        } else {
            if ((int)i == m_hoverItem) {
                HBRUSH hoverBrush = CreateSolidBrush(RGB(51, 51, 51));
                FillRect(hdc, &itemRect, hoverBrush);
                DeleteObject(hoverBrush);
            }

            SetTextColor(hdc, RGB(240, 240, 240));
            RECT textRect = itemRect;
            textRect.left += 35;
            DrawTextW(hdc, item.text.c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER);

            if (item.isChecked) {
                const WCHAR checkMark[] = L"\u2713";
                RECT checkRect = itemRect;
                checkRect.right = 30;
                DrawTextW(hdc, checkMark, 1, &checkRect, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            }
            if (item.isSubMenu) {
                const WCHAR arrow[] = L"\u25B6";
                RECT arrowRect = itemRect;
                arrowRect.left = m_width - 30;
                arrowRect.right = m_width - 10;
                DrawTextW(hdc, arrow, 1, &arrowRect, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            }
        }
        currentY += itemHeight;
    }
}