#include "pch.h"
#include "Menu.h"
#include "App.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <vector>
#include <algorithm>

const wchar_t POPUP_MENU_CLASS[] = L"VirtuaCamCustomMenu";

static CustomMenu* g_topLevelMenu = nullptr;
static std::vector<CustomMenu*> g_openMenus;

CustomMenu::CustomMenu(HWND parent, HINSTANCE instance) : m_parentHwnd(parent), m_instance(instance), m_hwnd(nullptr), m_parentMenu(nullptr), m_activeSubMenu(nullptr), m_activeSubMenuItem(-1) {
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
}

HWND CustomMenu::GetHwnd() const { return m_hwnd; }
void CustomMenu::AddItem(const std::wstring& text, UINT id, bool checked) { m_items.push_back({ text, id, false, checked, false, nullptr }); }
void CustomMenu::AddSeparator() { m_items.push_back({ L"", 0, true, false, false, nullptr }); }

CustomMenu* CustomMenu::AddSubMenu(const std::wstring& text) {
    auto newSubMenu = std::make_unique<CustomMenu>(m_parentHwnd, m_instance);
    newSubMenu->m_parentMenu = this;
    CustomMenu* rawPtr = newSubMenu.get();
    m_items.push_back({ text, 0, false, false, true, std::move(newSubMenu) });
    return rawPtr;
}

void CustomMenu::CalculateOptimalWidth() {
    if (m_calculatedWidth > 0) return;
    HDC hdc = GetDC(NULL);
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
    int maxWidth = 0;
    for (const auto& item : m_items) {
        if (!item.isSeparator) {
            SIZE size;
            if (GetTextExtentPoint32W(hdc, item.text.c_str(), (int)item.text.length(), &size)) {
                if (size.cx > maxWidth) maxWidth = size.cx;
            }
        }
    }
    SelectObject(hdc, hOldFont);
    ReleaseDC(NULL, hdc);
    m_calculatedWidth = maxWidth + 85;
}

int CustomMenu::GetCalculatedWidth() const {
    const_cast<CustomMenu*>(this)->CalculateOptimalWidth();
    return m_calculatedWidth;
}

int CustomMenu::GetCalculatedHeight() const {
    int height = 0;
    for (const auto& item : m_items) {
        height += item.isSeparator ? 10 : m_itemHeight;
    }
    return height;
}

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

    g_openMenus.push_back(this);
    if (m_parentMenu == nullptr) {
        g_topLevelMenu = this;
        SetCapture(m_hwnd);
    }

    DWM_SYSTEMBACKDROP_TYPE backdropType = DWMSBT_TRANSIENTWINDOW;
    DwmSetWindowAttribute(m_hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
    BOOL useDarkMode = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));

    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
}

void CustomMenu::CloseChildren() {
    if (m_activeSubMenu) {
        if(IsWindow(m_activeSubMenu->m_hwnd)) {
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
        int itemHeight = m_items[i].isSeparator ? 10 : m_itemHeight;
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
            for(int i = 0; i < m_hoverItem; ++i) {
                itemTop += m_items[i].isSeparator ? 10 : m_itemHeight;
            }
            rcItem.top = itemTop;
            rcItem.bottom = rcItem.top + m_itemHeight;
            ClientToScreen(m_hwnd, (POINT*)&rcItem.left);
            ClientToScreen(m_hwnd, (POINT*)&rcItem.right);

            m_activeSubMenu->CalculateOptimalWidth();
            int subMenuW = m_activeSubMenu->m_calculatedWidth;
            int subMenuH = 0;
            for (const auto& item : m_activeSubMenu->m_items) subMenuH += item.isSeparator ? 10 : m_itemHeight;

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
        Draw(hdc);
        EndPaint(hWnd, &ps);
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
                int itemHeight = item.isSeparator ? 10 : m_itemHeight;
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
        if (this == g_topLevelMenu) {
            ReleaseCapture();
            g_topLevelMenu = nullptr;
        }

        auto it = std::find(g_openMenus.begin(), g_openMenus.end(), this);
        if (it != g_openMenus.end()) {
            g_openMenus.erase(it);
        }
        m_hwnd = nullptr;
        SetWindowLongPtr(hWnd, GWLP_USERDATA, 0);
        return 0;
    }
    
    case WM_NCDESTROY: {
        delete this;
        return 0;
    }
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
        RECT itemRect = { 0, currentY, m_calculatedWidth, currentY + itemHeight };
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
                arrowRect.left = m_calculatedWidth - 30;
                arrowRect.right = m_calculatedWidth - 10;
                DrawTextW(hdc, arrow, 1, &arrowRect, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            }
        }
        currentY += itemHeight;
    }
}