#pragma once
#include <vector>
#include <string>
#include <functional>
#include <d3d11.h>

#define PREVIEW_WINDOW_CLASS L"VirtuaCamPreviewClass"

enum class BrokerState;

struct CapturableWindow {
    HWND hwnd;
    std::wstring title;
};

typedef ID3D11Texture2D* (*PFN_GetSharedTexture)();

void UI_Initialize(HINSTANCE instance, HWND& outMainWnd, PFN_GetSharedTexture pfnGetSharedTexture);
void UI_RunMessageLoop(std::function<void()> onIdle);
void UI_Shutdown();
void UI_UpdateAudioDeviceLists(const std::vector<std::wstring>& captureDevices);
void UI_SetAudioSelectionCallback(std::function<void(int)> callback);

// Shows a transient balloon notification on the tray icon (used for
// surfacing errors such as a producer process failing to launch).
void UI_ShowTrayNotification(const std::wstring& title, const std::wstring& message);

void CreatePreviewWindow();
void UpdateTelemetry(BrokerState currentState);