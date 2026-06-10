// =============================================================================
// App.h  --  Application-wide constants, IDs, enums, and shared state types
// =============================================================================
// This header is included by the main app (VirtuaCam.exe) and by the broker
// DLL (DirectPortBroker.dll) to share common types.
//
// Window message constants
// ------------------------
// WM_APP_TRAY_MSG    — Posted to the main window by the system-tray icon.
// WM_APP_MENU_COMMAND — Posted when a command is selected from our custom menu.
//
// Menu/control ID layout
// ----------------------
// IDs are allocated in blocks so the app can determine the *type* of a menu
// selection by range-checking the ID:
//
//  System-tray / window controls:  5001 – 5004
//  Audio device selection:         6000 (none), 7001+ (capture devices)
//
//  Source IDs follow a replicated pattern — one block per source slot:
//    Slot 0 = main source
//    Slot 1 = PiP top-left    (TL)
//    Slot 2 = PiP top-right   (TR)
//    Slot 3 = PiP bottom-left (BL)
//    Slot 4 = PiP bottom-right / all-PiP toggle
//
//  Within each slot the IDs are sub-divided by source type:
//    base + 0      = Off
//    base + 1      = Consumer (the loopback/demo producer)
//    base + 100..  = Physical cameras (one ID per camera index)
//    base + 1000.. = Capturable windows (one ID per HWND in enumeration order)
//    base + 1500.. = Discovered producers (one ID per process in discovery order)
//
//  Slot base addresses:
//    Main      8000
//    PiP TL   10000
//    PiP TR   12000
//    PiP BL   14000
//    PiP (BR/toggle) 16000
//
//  Settings items:  18001 – 18003
// =============================================================================

#pragma once

#include "Formats.h"

// --- System tray / window control IDs ---
#define WM_APP_TRAY_MSG         (WM_APP + 1)    // System-tray notification message
#define WM_APP_MENU_COMMAND     (WM_APP + 2)    // Custom menu selection forwarded to main WndProc
#define ID_TRAY_PREVIEW_WINDOW  5001            // "Preview" menu item
#define ID_TRAY_ABOUT           5002            // "About" menu item
#define ID_TRAY_EXIT            5003            // "Exit" menu item
#define IDC_TELEMETRY_LABEL     5004            // Static text showing broker state

// --- Audio device IDs ---
#define ID_AUDIO_DEVICE_NONE    6000            // "None" / disable audio capture
#define ID_AUDIO_CAPTURE_FIRST  7001            // First real capture device (7001, 7002, ...)

// =============================================================================
// Source selection IDs  (see layout table in the file comment above)
// =============================================================================

// --- Main source slot (base = 8000) ---
#define ID_SOURCE_OFF                   8000
#define ID_SOURCE_CONSUMER              8001
#define ID_SOURCE_CAMERA_FIRST          8100    // 8100 .. 8199 — one per camera
#define ID_SOURCE_WINDOW_FIRST          9000    // 9000 .. 9499 — one per enumerated window
#define ID_SOURCE_DISCOVERED_FIRST      9500    // 9500+         — one per discovered producer

// --- PiP top-left slot (base = 10000) ---
#define ID_PIP_TL_OFF                   10000
#define ID_PIP_TL_CONSUMER              10001
#define ID_PIP_TL_CAMERA_FIRST          10100
#define ID_PIP_TL_WINDOW_FIRST          11000
#define ID_PIP_TL_DISCOVERED_FIRST      11500

// --- PiP top-right slot (base = 12000) ---
#define ID_PIP_TR_OFF                   12000
#define ID_PIP_TR_CONSUMER              12001
#define ID_PIP_TR_CAMERA_FIRST          12100
#define ID_PIP_TR_WINDOW_FIRST          13000
#define ID_PIP_TR_DISCOVERED_FIRST      13500

// --- PiP bottom-left slot (base = 14000) ---
#define ID_PIP_BL_OFF                   14000
#define ID_PIP_BL_CONSUMER              14001
#define ID_PIP_BL_CAMERA_FIRST          14100
#define ID_PIP_BL_WINDOW_FIRST          15000
#define ID_PIP_BL_DISCOVERED_FIRST      15500

// --- PiP bottom-right / all-PiP toggle (base = 16000) ---
#define ID_PIP_OFF                      16000
#define ID_PIP_CONSUMER                 16001
#define ID_PIP_CAMERA_FIRST             16100
#define ID_PIP_WINDOW_FIRST             17000
#define ID_PIP_DISCOVERED_FIRST         17500

// --- Settings items ---
#define ID_SETTINGS_PIP_TL              18001   // Toggle PiP top-left visibility
#define ID_SETTINGS_PIP_TR              18002   // Toggle PiP top-right visibility
#define ID_SETTINGS_PIP_BL              18003   // Toggle PiP bottom-left visibility
#define ID_SETTINGS_AUTOSTART           18004   // Toggle "Start with Windows"

// =============================================================================
// Enumerations
// =============================================================================

// State of the broker's producer connection, reported to the UI for telemetry.
enum class BrokerState {
    Searching,  // No manifest found yet; no producer is currently running
    Connected,  // At least one producer is composited into the output
    Failed      // A manifest was found but texture/fence handles could not be opened
};

// Commands that the main app can write into the broker's output manifest to
// request behaviour changes.  Currently only None is used.
enum class VCamCommand {
    None = 0
};

// Which type of input source a slot is set to.
enum class SourceMode {
    Off,        // Slot is disabled (contributes nothing to the composite)
    Consumer,   // The loopback/demo consumer producer
    Camera,     // A physical webcam (identified by cameraIndex)
    Discovered, // A dynamically discovered producer process (identified by pid)
    Window      // A captured application window (identified by hwnd)
};

// The four picture-in-picture corner positions.
enum class PipPosition { TL, TR, BL, BR };

// Describes the current selection for one source slot (main or any PiP corner).
struct SourceState {
    SourceMode mode        = SourceMode::Off;  // Which source type is active
    DWORD      pid         = 0;                // PID of a Discovered producer (if mode == Discovered)
    HWND       hwnd        = nullptr;          // Target window handle (if mode == Window)
    int        cameraIndex = -1;               // Index into the camera enumeration list (if mode == Camera)
};
