// =============================================================================
// TrayApp.h  --  The VirtuaCam tray application
// =============================================================================
// Owns the pieces and wires them together:
//
//   tray menu ──► slot choices ──► ProducerHost (producer processes)
//                              └─► Broker layout (by producer pid)
//   Broker ──► shared frame ──► VirtuaCamSource.dll in the Camera Frame Server
//   Broker ──► PreviewWindow / menu thumbnail (only while visible)
//
// Everything runs off window messages; the UI thread sleeps in GetMessage.
// =============================================================================

#pragma once

#include "Broker.h"
#include "PreviewWindow.h"
#include "ProducerHost.h"
#include "Settings.h"

#include <mfvirtualcamera.h>
#include <functional>
#include <string>
#include <vector>

class PopupMenu;

class TrayApp {
public:
    int Run(HINSTANCE instance);

private:
    enum Slot { MainSlot = 0, TopLeft, TopRight, BottomLeft, BottomRight, SlotCount };

    struct Choice {
        enum class Kind { Off, Producer, External } kind = Kind::Off;
        ProducerSpec producer;      // Kind::Producer
        DWORD externalPid = 0;      // Kind::External (someone else's producer)
        std::wstring label;
        bool operator==(const Choice& o) const {
            return kind == o.kind && producer == o.producer && externalPid == o.externalPid;
        }
    };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    HRESULT StartVirtualCamera();
    bool RegisterCameraComponent();
    void AddTrayIcon();
    void Notify(const std::wstring& title, const std::wstring& text);
    void UpdateStatus();

    void ShowMenu(POINT anchor);
    void AddSourceItems(PopupMenu* menu, int slot);
    UINT Command(std::function<void()> action);
    void Assign(int slot, Choice choice);
    void SetGrid(bool grid);
    void PushLayout();
    void OnProducerExited(DWORD pid);
    void OnFrame();
    void UpdateFrameNotifications();

    HINSTANCE m_instance = nullptr;
    HWND m_hwnd = nullptr;
    UINT m_taskbarCreated = 0;
    Settings m_settings{};
    Broker m_broker;
    ProducerHost m_producers;
    PreviewWindow m_preview;
    wil::com_ptr_nothrow<IMFVirtualCamera> m_camera;

    Choice m_slots[SlotCount];
    DWORD m_slotPids[SlotCount] = {};
    bool m_grid = false;
    std::vector<std::function<void()>> m_commands;
    std::wstring m_status;
};
