#pragma once

#include "Formats.h"

#define WM_APP_TRAY_MSG (WM_APP + 1)
#define ID_TRAY_PREVIEW_WINDOW  5001
#define ID_TRAY_ABOUT           5002
#define ID_TRAY_EXIT            5003
#define IDC_TELEMETRY_LABEL     5004
#define ID_AUDIO_DEVICE_NONE    6000
#define ID_AUDIO_RENDER_FIRST   6001
#define ID_AUDIO_CAPTURE_FIRST  7001

#define ID_SOURCE_PASSTHROUGH           8000
#define ID_SOURCE_CAMERA                8001
#define ID_SOURCE_DISCOVERED_FIRST      8002

enum class BrokerState { Searching, Connected, Failed };

enum class VCamCommand {
    None = 0,
};

enum class SourceMode { Passthrough, Camera, Discovered };