#pragma once

#include "Formats.h"

#define WM_APP_TRAY_MSG (WM_APP + 1)
#define WM_APP_MENU_COMMAND (WM_APP + 2)
#define ID_TRAY_PREVIEW_WINDOW  5001
#define ID_TRAY_ABOUT           5002
#define ID_TRAY_EXIT            5003
#define IDC_TELEMETRY_LABEL     5004
#define ID_AUDIO_DEVICE_NONE    6000
#define ID_AUDIO_CAPTURE_FIRST  7001

#define ID_SOURCE_OFF                   8000
#define ID_SOURCE_CONSUMER              8001
#define ID_SOURCE_CAMERA_FIRST          8100
#define ID_SOURCE_WINDOW_FIRST          9000
#define ID_SOURCE_DISCOVERED_FIRST      9500

#define ID_PIP_TL_OFF                   10000
#define ID_PIP_TL_CONSUMER              10001
#define ID_PIP_TL_CAMERA_FIRST          10100
#define ID_PIP_TL_WINDOW_FIRST          11000
#define ID_PIP_TL_DISCOVERED_FIRST      11500

#define ID_PIP_TR_OFF                   12000
#define ID_PIP_TR_CONSUMER              12001
#define ID_PIP_TR_CAMERA_FIRST          12100
#define ID_PIP_TR_WINDOW_FIRST          13000
#define ID_PIP_TR_DISCOVERED_FIRST      13500

#define ID_PIP_BL_OFF                   14000
#define ID_PIP_BL_CONSUMER              14001
#define ID_PIP_BL_CAMERA_FIRST          14100
#define ID_PIP_BL_WINDOW_FIRST          15000
#define ID_PIP_BL_DISCOVERED_FIRST      15500

#define ID_PIP_OFF                      16000
#define ID_PIP_CONSUMER                 16001
#define ID_PIP_CAMERA_FIRST             16100
#define ID_PIP_WINDOW_FIRST             17000
#define ID_PIP_DISCOVERED_FIRST         17500

#define ID_SETTINGS_PIP_TL              18001
#define ID_SETTINGS_PIP_TR              18002
#define ID_SETTINGS_PIP_BL              18003

enum class BrokerState { Searching, Connected, Failed };
enum class VCamCommand { None = 0 };
enum class SourceMode { Off, Consumer, Camera, Discovered, Window };
enum class PipPosition { TL, TR, BL, BR };

struct SourceState {
    SourceMode mode = SourceMode::Off;
    DWORD pid = 0;
    HWND hwnd = nullptr;
    int cameraIndex = -1;
};