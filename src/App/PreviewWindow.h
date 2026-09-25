// =============================================================================
// PreviewWindow.h  --  Resizable window showing exactly what the camera sends
// =============================================================================

#pragma once

#include "Presenter.h"

#include <string>

class Broker;

class PreviewWindow {
public:
    void Open(HINSTANCE instance, Broker* broker);
    void Close();
    bool IsOpen() const { return m_hwnd != nullptr; }

    // Called when the broker has a new frame or status.
    void Refresh();
    void SetStatus(const std::wstring& status);

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    HWND m_hwnd = nullptr;
    Broker* m_broker = nullptr;
    Presenter m_presenter;
};
