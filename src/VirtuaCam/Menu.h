#pragma once
#include <cstdint>
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
    bool isPreview = false;   // Live video thumbnail item (see SetPreviewProvider)
    std::unique_ptr<CustomMenu> subMenu;
};

// Fills `bgra` with a top-down 32-bit BGRA frame (alpha forced opaque) and
// reports its dimensions.  Returns false when no frame is available.
using MenuPreviewProvider = std::function<bool(std::vector<uint32_t>& bgra, UINT& width, UINT& height)>;

class CustomMenu {
public:
    CustomMenu(HWND parent, HINSTANCE instance);
    ~CustomMenu();

    void AddItem(const std::wstring& text, UINT id, bool checked = false);
    void AddSeparator();
    // Adds a live preview thumbnail; clicking it sends `id` like a normal item.
    void AddPreviewItem(UINT id);
    CustomMenu* AddSubMenu(const std::wstring& text);
    void Show(int x, int y);
    HWND GetHwnd() const;
    static void CloseAllMenus();
    // Source of preview frames (set once at startup by the UI layer).
    static void SetPreviewProvider(MenuPreviewProvider provider);

    int GetCalculatedWidth() const;
    int GetCalculatedHeight() const;
    
    // VOM-style handle registration for deterministic cleanup
    void RegisterHandle();
    void UnregisterHandle();
    UINT GetHandleId() const { return m_handleId; }
    
    // Process-wide cleanup for handle table
    static void CleanupHandles();

private:
    void CalculateOptimalWidth();
    int ItemHeight(const CustomMenuItem& item) const;
    static LRESULT CALLBACK MenuWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    void Draw(HDC hdc, HANDLE paintBuffer);   // HANDLE == HPAINTBUFFER (uxtheme)
    void CloseChildren();
    void HandleMouseMove(POINT clientPt);
    static void SignalCloseEvent(UINT handleId);

    HWND m_hwnd;
    HWND m_parentHwnd;
    HINSTANCE m_instance;
    std::vector<CustomMenuItem> m_items;

    int m_itemHeight = 32;
    mutable int m_calculatedWidth = 0;
    int m_hoverItem = -1;
    bool m_hasPreview = false;

    CustomMenu* m_parentMenu = nullptr;
    CustomMenu* m_activeSubMenu = nullptr;
    int m_activeSubMenuItem = -1;
    
    // VOM handle tracking
    UINT m_handleId;
    UINT m_generation;
};
