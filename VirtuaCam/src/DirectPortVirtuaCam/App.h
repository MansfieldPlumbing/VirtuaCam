#pragma once
#include <SDKDDKVer.h>
#include "Resource.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <strsafe.h>
#include <initguid.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>
#include <shellapi.h>
#include <dxgi1_2.h>
#include <string>
#include <format>
#include "wil/result.h"
#include "wil/stl.h"
#include "wil/win32_helpers.h"
#include "wil/com.h"
#pragma comment(lib, "mfsensorgroup")
#pragma comment(lib, "comctl32")
#pragma comment(lib, "shell32.lib")
#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#define WM_APP_TRAY_MSG (WM_APP + 1)
#define ID_TRAY_PREVIEW_WINDOW  5001
#define ID_TRAY_ABOUT           5002
#define ID_TRAY_EXIT            5003
#define IDC_TELEMETRY_LABEL     5004
std::wstring to_wstring(const std::string& s);
const std::wstring GUID_ToStringW(const GUID& guid);
void CenterWindow(HWND hwnd, bool useCursorPos = true);
enum class BrokerState { Searching, Connected, Failed };