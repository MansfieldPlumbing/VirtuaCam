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
// Windows Runtime (WinRT / C++/WinRT)
// ---------------------------------------------------------------------------
#include "winrt/base.h"                                             // Core WinRT (com_ptr, hstring, etc.)
#include "winrt/Windows.ApplicationModel.h"                        // Package identity queries
#include <winrt/Windows.Foundation.h>                              // IAsyncOperation, IClosable, etc.
#include <winrt/Windows.Graphics.Capture.h>                        // GraphicsCaptureSession / Item
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>             // IDirect3DDevice (WinRT wrapper)
#include <windows.graphics.capture.interop.h>                      // IGraphicsCaptureItemInterop (HWND -> CaptureItem)
#include <windows.graphics.directx.direct3d11.interop.h>           // CreateDirect3D11DeviceFromDXGIDevice

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

// ---------------------------------------------------------------------------
// Undocumented / internal Media Foundation interfaces
// ---------------------------------------------------------------------------
// Windows' Media Foundation frame-server queries the virtual camera source for
// these interfaces via QueryInterface.  They are undocumented and not shipped
// in any public SDK header; we declare them here with their known IIDs so that:
//   1. Our QI implementation can return E_NOINTERFACE cleanly (rather than
//      crashing on a missing vtable).
//   2. The GUID resolver in Tools.cpp (GUID_ToStringW) can print a readable
//      name instead of a raw {xxxxxxxx-...} string in debug traces.
// None of these interfaces are implemented — they are declaration-only.
DECLARE_INTERFACE_IID_(IMFDeviceController,      IUnknown, "A1F58958-A5AA-412F-AF20-1B7F1242DBA0") {};
DECLARE_INTERFACE_IID_(IMFDeviceController2,     IUnknown, "2032C7EF-76F6-492A-94F3-4A81F69380CC") {};
DECLARE_INTERFACE_IID_(IMFDeviceTransformManager,IUnknown, "70212999-c449-4b9d-b1a4-b358e1490121") {};
DECLARE_INTERFACE_IID_(IMFDeviceSourceInternal,  IUnknown, "7F02A37E-4E81-11E0-8F3E-D057DFD72085") {};
DECLARE_INTERFACE_IID_(IMFDeviceSourceInternal2, IUnknown, "c47d95d5-9685-4bf6-b6fb-772dc58d8e3b") {};
DECLARE_INTERFACE_IID_(IMFDeviceSourceStatus,    IUnknown, "43937DC1-0BE6-4ADD-8A14-9EA68FF31252") {};
DECLARE_INTERFACE_IID_(IUndocumented1,           IUnknown, "9A9DAAAA-9774-4732-848E-8739655F2BA3") {};

// ---------------------------------------------------------------------------
// WinRT QueryInterface hierarchy teaching
// ---------------------------------------------------------------------------
// C++/WinRT's com_ptr::as<T>() uses is_guid_of<T>() to verify interface IDs.
// The MF interfaces form inheritance chains that WinRT doesn't know about by
// default.  These specialisations teach it the correct base-interface lists so
// that, for example, as<IMFMediaSource2>() will accept an IMFMediaSource QI hit.
namespace winrt
{
    template<> inline bool is_guid_of<IMFMediaSourceEx>(guid const& id) noexcept
    {
        return is_guid_of<IMFMediaSourceEx, IMFMediaSource, IMFMediaEventGenerator>(id);
    }
    template<> inline bool is_guid_of<IMFMediaSource2>(guid const& id) noexcept
    {
        return is_guid_of<IMFMediaSource2, IMFMediaSourceEx, IMFMediaSource, IMFMediaEventGenerator>(id);
    }
    template<> inline bool is_guid_of<IMFMediaStream2>(guid const& id) noexcept
    {
        return is_guid_of<IMFMediaStream2, IMFMediaStream, IMFMediaEventGenerator>(id);
    }
    template<> inline bool is_guid_of<IMFActivate>(guid const& id) noexcept
    {
        return is_guid_of<IMFActivate, IMFAttributes>(id);
    }
}

#endif
