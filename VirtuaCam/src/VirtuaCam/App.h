#pragma once

#include "Formats.h"

#define WM_APP_TRAY_MSG (WM_APP + 1)
#define WM_APP_MENU_COMMAND (WM_APP + 2)
#define ID_TRAY_PREVIEW_WINDOW  5001
#define ID_TRAY_ABOUT           5002
#define ID_TRAY_EXIT            5003
#define IDC_TELEMETRY_LABEL     5004
#define ID_AUDIO_DEVICE_NONE    6000
#define ID_AUDIO_RENDER_FIRST   6001
#define ID_AUDIO_CAPTURE_FIRST  7001

#define ID_SOURCE_OFF                   8000
#define ID_SOURCE_PASSTHROUGH           8001
#define ID_SOURCE_DISCOVERED_FIRST      8002
#define ID_SOURCE_HW_CAMERA_FIRST       8100
#define ID_SOURCE_WINDOW_FIRST          9000
#define ID_SOURCE_MULTIPLEXER_FIRST     9500

#define ID_PIP_OFF                      10000
#define ID_PIP_DISCOVERED_FIRST         10002
#define ID_PIP_HW_CAMERA_FIRST          10100
#define ID_PIP_WINDOW_FIRST             11000
#define ID_PIP_MULTIPLEXER_FIRST        11500

enum class BrokerState { Searching, Connected, Failed };

enum class SourceMode { OFF, Passthrough, Discovered, Window, HardwareCamera, Multiplexer };

int GetSelectedCameraId();
int GetSelectedPipCameraId();
HWND GetCapturedPipHwnd();
DWORD GetPreferredPipPID();
SourceMode GetPipSourceMode();
void SetPipMode(SourceMode newMode, DWORD_PTR context = 0);