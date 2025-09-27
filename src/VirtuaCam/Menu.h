#pragma once
#include <vector>
#include <string>
#include <memory>
#include <functional>

class CustomMenu;

struct CustomMenuItem {
    std::wstring text;
    UINT id;
    bool isSeparator = false;
    bool isChecked = false;
    bool isSubMenu = false;
    std::unique_ptr<CustomMenu> subMenu;
};

class CustomMenu {
public:
    CustomMenu(HWND parent, HINSTANCE instance);
    ~CustomMenu();

    void AddItem(const std::wstring& text, UINT id, bool checked = false);
    void AddSeparator();
    CustomMenu* AddSubMenu(const std::wstring& text);
    void Show(int x, int y);
    HWND GetHwnd() const;
    static void CloseAllMenus();
    
    int GetCalculatedWidth() const;
    int GetCalculatedHeight() const;

private:
    void CalculateOptimalWidth();
    static LRESULT CALLBACK MenuWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    void Draw(HDC hdc);
    void CloseChildren();
    void HandleMouseMove(POINT clientPt);

    HWND m_hwnd;
    HWND m_parentHwnd;
    HINSTANCE m_instance;
    std::vector<CustomMenuItem> m_items;
    
    int m_itemHeight = 32;
    int m_width = 250;
    mutable int m_calculatedWidth = 0;
    int m_hoverItem = -1;

    CustomMenu* m_parentMenu = nullptr;
    CustomMenu* m_activeSubMenu = nullptr;
    int m_activeSubMenuItem = -1;
};