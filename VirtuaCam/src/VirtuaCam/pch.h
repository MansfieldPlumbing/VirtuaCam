#pragma once
#ifndef PCH_H
#define PCH_H

#include "targetver.h"

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
#include <ntstatus.h>
#include <dxgi1_6.h>
#include <d3d11_4.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <uuids.h>
#include <wrl/client.h>
#include <wrl.h>
#include <sddl.h>
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
#include "wil/registry.h"
#include "winrt/base.h"

#pragma comment(lib, "mfsensorgroup")
#pragma comment(lib, "comctl32")
#pragma comment(lib, "shell32.lib")
#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include "Resource.h"
#include "Utilities.h"

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

    template<> inline bool is_guid_of<IMFActivate>(guid const& id) noexcept
    {
        return is_guid_of<IMFActivate, IMFAttributes>(id);
    }
}

#endif