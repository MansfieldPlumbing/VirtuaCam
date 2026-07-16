// =============================================================================
// pch.h  --  Precompiled Header (shared across all VirtuaCam targets)
// =============================================================================
// Everything included here is compiled once and reused by every translation
// unit.  Keep this file stable (infrequent changes) to maximise PCH hits and
// keep incremental build times short.
// =============================================================================

#pragma once
#ifndef PCH_H
#define PCH_H

// Trim down the Windows.h surface area to reduce compile time.
#define WIN32_LEAN_AND_MEAN
// Keep windows.h from defining min/max macros that break std::min/std::max.
#define NOMINMAX

// ---------------------------------------------------------------------------
// Debug heap  (debug builds only)
// ---------------------------------------------------------------------------
// DBG_NEW replaces 'new' in debug builds to record the source file and line
// number of every allocation, making memory-leak reports actionable.
#define _CRTDBG_MAP_ALLOC
#include <cstdlib>
#include <crtdbg.h>
#ifdef _DEBUG
#define DBG_NEW new ( _NORMAL_BLOCK , __FILE__ , __LINE__ )
#else
#define DBG_NEW new
#endif

// ---------------------------------------------------------------------------
// Windows core
// ---------------------------------------------------------------------------
#include <windows.h>
#include <objbase.h>        // CoInitializeEx, COM helpers
#include <inspectable.h>    // IInspectable (WinRT base interface)
#include <commctrl.h>       // Common Controls (toolbar, status bar, etc.)
#include <shellapi.h>       // Shell_NotifyIcon (system-tray)
#include <strsafe.h>        // Safe string functions (StringCchCopy etc.)
#include <propvarutil.h>    // PROPVARIANT helpers

// ---------------------------------------------------------------------------
// Media Foundation & Kernel Streaming
// ---------------------------------------------------------------------------
#include <mfapi.h>              // Core MF API (MFCreateMediaType, etc.)
#include <mfidl.h>              // IMFMediaSource, IMFMediaStream, IMFActivate, ...
#include <mfvirtualcamera.h>    // IMFVirtualCamera, sensor-profile APIs
#include <mferror.h>            // MF_E_* error codes
#include <mfcaptureengine.h>    // IMFCaptureEngine (used for profile queries)
#include <ks.h>                 // Kernel Streaming base types (KSIDENTIFIER, etc.)
#include <ksproxy.h>            // IKsControl (KS property get/set from user mode)
#include <ksmedia.h>            // KSPROPSETID_*, PROPSETID_VIDCAP_* property sets

// ---------------------------------------------------------------------------
// DirectX / DXGI
// ---------------------------------------------------------------------------
#include <dxgi1_6.h>        // IDXGIFactory6, IDXGISwapChain4, adapter enumeration
#include <d3d11_4.h>        // ID3D11Device5, ID3D11DeviceContext4, ID3D11Fence
#include <d2d1_1.h>         // Direct2D (used for 2D UI rendering)
#include <dwrite.h>         // DirectWrite (text layout and rendering)
#include <wincodec.h>       // Windows Imaging Component (bitmap encode/decode)
#include <uuids.h>          // Media-type GUIDs (MFVideoFormat_RGB32, etc.)

// ---------------------------------------------------------------------------
// C++ Standard Library
// ---------------------------------------------------------------------------
#include <string>
#include <format>       // std::format (C++20)
#include <vector>
#include <map>
#include <cassert>
#include <memory>
#include <functional>

// ---------------------------------------------------------------------------
// WIL  (Windows Implementation Library)
// ---------------------------------------------------------------------------
// Provides RETURN_IF_FAILED, THROW_IF_FAILED, wil::com_ptr_nothrow<T>,
// wil::unique_handle, and many other ergonomic Win32/COM helpers.
#include "wil/result.h"
#include "wil/stl.h"
#include "wil/win32_helpers.h"
#include "wil/com.h"

// ---------------------------------------------------------------------------
// Manifest dependency: Common Controls v6 (for visual styles)
// ---------------------------------------------------------------------------
#pragma comment(lib, "mfsensorgroup")
#pragma comment(lib, "comctl32")
#pragma comment(lib, "shell32.lib")
#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---------------------------------------------------------------------------
// Project headers (must come after Windows/DirectX headers)
// ---------------------------------------------------------------------------
#include "Resource.h"
#include "Guids.h"

#endif
