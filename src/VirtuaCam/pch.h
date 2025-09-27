#pragma once
#ifndef PCH_H
#define PCH_H

#define WIN32_LEAN_AND_MEAN

#define _CRTDBG_MAP_ALLOC
#include <cstdlib>
#include <crtdbg.h>
#ifdef _DEBUG
#define DBG_NEW new ( _NORMAL_BLOCK , __FILE__ , __LINE__ )
#else
#define DBG_NEW new
#endif

#include <windows.h>
#include <objbase.h>
#include <inspectable.h>
#include <commctrl.h>
#include <shellapi.h>
#include <strsafe.h>
#include <propvarutil.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>
#include <mferror.h>
#include <mfcaptureengine.h>
#include <ks.h>
#include <ksproxy.h>
#include <ksmedia.h>
#include <dxgi1_6.h>
#include <d3d11_4.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <uuids.h>

#include "winrt/base.h"
#include "winrt/Windows.ApplicationModel.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <string>
#include <format>
#include <vector>
#include <map>
#include <cassert>
#include <memory>
#include <functional>

#include "wil/result.h"
#include "wil/stl.h"
#include "wil/win32_helpers.h"
#include "wil/com.h"

#pragma comment(lib, "mfsensorgroup")
#pragma comment(lib, "comctl32")
#pragma comment(lib, "shell32.lib")
#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include "Resource.h"
#include "Guids.h"

DECLARE_INTERFACE_IID_(IMFDeviceController, IUnknown, "A1F58958-A5AA-412F-AF20-1B7F1242DBA0") {};
DECLARE_INTERFACE_IID_(IMFDeviceController2, IUnknown, "2032C7EF-76F6-492A-94F3-4A81F69380CC") {};
DECLARE_INTERFACE_IID_(IMFDeviceTransformManager, IUnknown, "70212999-c449-4b9d-b1a4-b358e1490121") {};
DECLARE_INTERFACE_IID_(IMFDeviceSourceInternal, IUnknown, "7F02A37E-4E81-11E0-8F3E-D057DFD72085") {};
DECLARE_INTERFACE_IID_(IMFDeviceSourceInternal2, IUnknown, "c47d95d5-9685-4bf6-b6fb-772dc58d8e3b") {};
DECLARE_INTERFACE_IID_(IMFDeviceSourceStatus, IUnknown, "43937DC1-0BE6-4ADD-8A14-9EA68FF31252") {};
DECLARE_INTERFACE_IID_(IUndocumented1, IUnknown, "9A9DAAAA-9774-4732-848E-8739655F2BA3") {};

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