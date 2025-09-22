#pragma once
#include <vector>
#include <string>
#include <functional>
#include <d3d11.h>

#define PREVIEW_WINDOW_CLASS L"VirtuaCamPreviewClass"

enum class BrokerState;

typedef HANDLE (*PFN_GetSharedTextureHandle)();

void UI_Initialize(HINSTANCE instance, ID3D11Device* pDevice, HWND& outMainWnd, PFN_GetSharedTextureHandle pfnGetSharedTextureHandle);
void UI_RunMessageLoop(std::function<void()> onIdle);
void UI_Shutdown();
void UI_UpdateAudioDeviceLists(const std::vector<std::wstring>& captureDevices);
void UI_SetAudioSelectionCallback(std::function<void(int)> callback);

void CreatePreviewWindow();
void UpdateTelemetry(BrokerState currentState);