#include "pch.h"
#include "Guids.h"
#include "Utilities.h"
#include <d3d12.h>
#include <map>
#include <functional>
#include <numeric>
#include <string>
#include <vector>
#include <format>

namespace
{
    struct CompareGuids {
        bool operator()(const GUID& a, const GUID& b) const {
            return std::memcmp(&a, &b, sizeof(GUID)) < 0;
        }
    };

    std::wstring lookupName(const std::map<DWORD, const wchar_t*>& map, DWORD id) {
        if (auto it = map.find(id); it != map.end()) {
            return it->second;
        }
        return std::to_wstring(id);
    }

    std::wstring lookupFlags(const std::map<DWORD, const wchar_t*>& map, DWORD flags) {
        if (flags == 0) return L"0";
        
        return std::accumulate(
            map.begin(), map.end(), std::wstring{},
            [flags](std::wstring current, const auto& pair) {
                if ((flags & pair.first) == pair.first && pair.first != 0) {
                    if (!current.empty()) current += L" | ";
                    current += pair.second;
                }
                return current;
            }
        );
    }
} 

namespace VirtuaCam::Utils
{
    namespace Debug
    {
        std::wstring GuidToWString(const GUID& guid, bool resolveKnown)
        {
            if (resolveKnown) {
                static const std::map<GUID, std::wstring, CompareGuids> s_knownGuids = {
                    { GUID_NULL, L"GUID_NULL" },
                    { CLSID_VirtuaCam, L"CLSID_VirtuaCam" },
                    { PINNAME_VIDEO_CAPTURE, L"PINNAME_VIDEO_CAPTURE" },
                    { MF_MT_MAJOR_TYPE, L"MF_MT_MAJOR_TYPE" },
                    { MFMediaType_Video, L"MFMediaType_Video" },
                    { MFVideoFormat_RGB32, L"MFVideoFormat_RGB32" },
                    { MFVideoFormat_NV12, L"MFVideoFormat_NV12" },
                    { MF_MT_SUBTYPE, L"MF_MT_SUBTYPE" }
                };
                if (auto it = s_knownGuids.find(guid); it != s_knownGuids.end()) {
                    return it->second;
                }
            }

            wchar_t guidString[40] = { 0 };
            StringFromGUID2(guid, guidString, _countof(guidString));
            return guidString;
        }

        std::wstring KsPropertyToWString(PKSIDENTIFIER identifier, ULONG length)
        {
            if (!identifier || length < sizeof(KSIDENTIFIER)) {
                return L"<invalid_identifier>";
            }

            static const auto& s_ksPropertySets = []() -> const auto& {
                static const std::map<GUID, std::pair<const wchar_t*, std::map<DWORD, const wchar_t*>>, CompareGuids> propertyMap = {
                    { KSPROPSETID_ExtendedCameraControl, { L"ExtendedCameraControl", {
                        { KSPROPERTY_CAMERACONTROL_EXTENDED_PHOTOMODE, L"PHOTOMODE" }, { KSPROPERTY_CAMERACONTROL_EXTENDED_FOCUSMODE, L"FOCUSMODE" },
                        { KSPROPERTY_CAMERACONTROL_EXTENDED_EVCOMPENSATION, L"EVCOMPENSATION" }, { KSPROPERTY_CAMERACONTROL_EXTENDED_ISO, L"ISO" },
                        { KSPROPERTY_CAMERACONTROL_EXTENDED_ZOOM, L"ZOOM" }, { KSPROPERTY_CAMERACONTROL_EXTENDED_FACEDETECTION, L"FACEDETECTION" } } }
                    },
                    { PROPSETID_VIDCAP_CAMERACONTROL, { L"VIDCAP_CAMERACONTROL", {
                        { KSPROPERTY_CAMERACONTROL_PAN, L"PAN" }, { KSPROPERTY_CAMERACONTROL_TILT, L"TILT" }, { KSPROPERTY_CAMERACONTROL_ROLL, L"ROLL" },
                        { KSPROPERTY_CAMERACONTROL_ZOOM, L"ZOOM" }, { KSPROPERTY_CAMERACONTROL_EXPOSURE, L"EXPOSURE" }, { KSPROPERTY_CAMERACONTROL_IRIS, L"IRIS" },
                        { KSPROPERTY_CAMERACONTROL_FOCUS, L"FOCUS" } } }
                    },
                    { PROPSETID_VIDCAP_VIDEOPROCAMP, { L"VIDCAP_VIDEOPROCAMP", {
                        { KSPROPERTY_VIDEOPROCAMP_BRIGHTNESS, L"BRIGHTNESS" }, { KSPROPERTY_VIDEOPROCAMP_CONTRAST, L"CONTRAST" }, { KSPROPERTY_VIDEOPROCAMP_HUE, L"HUE" },
                        { KSPROPERTY_VIDEOPROCAMP_SATURATION, L"SATURATION" }, { KSPROPERTY_VIDEOPROCAMP_SHARPNESS, L"SHARPNESS" }, { KSPROPERTY_VIDEOPROCAMP_GAMMA, L"GAMMA" },
                        { KSPROPERTY_VIDEOPROCAMP_WHITEBALANCE, L"WHITEBALANCE" }, { KSPROPERTY_VIDEOPROCAMP_BACKLIGHT_COMPENSATION, L"BACKLIGHT_COMPENSATION" },
                        { KSPROPERTY_VIDEOPROCAMP_GAIN, L"GAIN" } } }
                    },
                    { KSPROPSETID_Pin, { L"Pin", {
                        { KSPROPERTY_PIN_CINSTANCES, L"CINSTANCES" }, { KSPROPERTY_PIN_CTYPES, L"CTYPES" }, { KSPROPERTY_PIN_DATAFLOW, L"DATAFLOW" },
                        { KSPROPERTY_PIN_DATARANGES, L"DATARANGES" }, { KSPROPERTY_PIN_DATAINTERSECTION, L"DATAINTERSECTION" }, { KSPROPERTY_PIN_INTERFACES, L"INTERFACES" },
                        { KSPROPERTY_PIN_MEDIUMS, L"MEDIUMS" }, { KSPROPERTY_PIN_CATEGORY, L"CATEGORY" }, { KSPROPERTY_PIN_NAME, L"NAME" } } }
                    }
                };
                return propertyMap;
            }();
            
            static const std::map<DWORD, const wchar_t*> s_ksPropertyTypeMap = {
                { KSPROPERTY_TYPE_GET, L"GET" }, { KSPROPERTY_TYPE_SET, L"SET" }, { KSPROPERTY_TYPE_SETSUPPORT, L"SETSUPPORT" },
                { KSPROPERTY_TYPE_BASICSUPPORT, L"BASICSUPPORT" }, { KSPROPERTY_TYPE_RELATIONS, L"RELATIONS" }, { KSPROPERTY_TYPE_SERIALIZESET, L"SERIALIZESET" },
                { KSPROPERTY_TYPE_UNSERIALIZESET, L"UNSERIALIZESET" }, { KSPROPERTY_TYPE_SERIALIZERAW, L"SERIALIZERAW" }, { KSPROPERTY_TYPE_UNSERIALIZERAW, L"UNSERIALIZERAW" },
                { KSPROPERTY_TYPE_SERIALIZESIZE, L"SERIALIZESIZE" }, { KSPROPERTY_TYPE_DEFAULTVALUES, L"DEFAULTVALUES" }, { KSPROPERTY_TYPE_TOPOLOGY, L"TOPOLOGY" }
            };

            std::wstring setName, propName;
            if (auto it = s_ksPropertySets.find(identifier->Set); it != s_ksPropertySets.end()) {
                setName = it->second.first;
                propName = lookupName(it->second.second, identifier->Id);
            } else {
                setName = GuidToWString(identifier->Set, false);
                propName = std::to_wstring(identifier->Id);
            }

            std::wstring flagsName = lookupFlags(s_ksPropertyTypeMap, identifier->Flags);
            return std::format(L"{} :: {} ({})", setName, propName, flagsName);
        }
        
        std::wstring PropVariantToWString(const PROPVARIANT& propertyVariant)
        {
            if (propertyVariant.vt == VT_CLSID && propertyVariant.puuid != nullptr)
            {
                return GuidToWString(*propertyVariant.puuid, true);
            }

            wil::unique_cotaskmem_string allocatedString;
            if (SUCCEEDED(PropVariantToStringAlloc(propertyVariant, &allocatedString)))
            {
                return allocatedString.get();
            }
            return L"<unparsable_propvariant>";
        }
    }

    namespace String
    {
        std::wstring FromUtf8(const std::string& utf8String)
        {
            if (utf8String.empty()) {
                return {};
            }
            int wideCharCount = MultiByteToWideChar(CP_UTF8, 0, utf8String.c_str(), static_cast<int>(utf8String.length()), nullptr, 0);
            if (wideCharCount == 0) {
                return {};
            }
            std::wstring wideString(wideCharCount, 0);
            MultiByteToWideChar(CP_UTF8, 0, utf8String.c_str(), static_cast<int>(utf8String.length()), &wideString[0], wideCharCount);
            return wideString;
        }

        std::string ToUtf8(const std::wstring& wideString)
        {
            if (wideString.empty()) {
                return {};
            }
            int byteCount = WideCharToMultiByte(CP_UTF8, 0, wideString.c_str(), static_cast<int>(wideString.length()), nullptr, 0, nullptr, nullptr);
            if (byteCount == 0) {
                return {};
            }
            std::string utf8String(byteCount, 0);
            WideCharToMultiByte(CP_UTF8, 0, wideString.c_str(), static_cast<int>(wideString.length()), &utf8String[0], byteCount, nullptr, nullptr);
            return utf8String;
        }
    }

    namespace Win32
    {
        void CenterWindowRelativeToCursor(HWND windowHandle)
        {
            if (!IsWindow(windowHandle))
            {
                return;
            }

            POINT cursorPoint;
            if (!GetCursorPos(&cursorPoint))
            {
                return;
            }

            HMONITOR currentMonitor = MonitorFromPoint(cursorPoint, MONITOR_DEFAULTTONEAREST);

            MONITORINFO monitorDetails = { sizeof(monitorDetails) };
            if (!GetMonitorInfo(currentMonitor, &monitorDetails))
            {
                return;
            }

            RECT windowRect;
            if (!GetWindowRect(windowHandle, &windowRect))
            {
                return;
            }

            const RECT& workArea = monitorDetails.rcWork;
            long monitorCenterX = workArea.left + (workArea.right - workArea.left) / 2;
            long monitorCenterY = workArea.top + (workArea.bottom - workArea.top) / 2;

            long windowHalfWidth = (windowRect.right - windowRect.left) / 2;
            long windowHalfHeight = (windowRect.bottom - windowRect.top) / 2;

            int newLeft = monitorCenterX - windowHalfWidth;
            int newTop = monitorCenterY - windowHalfHeight;

            SetWindowPos(windowHandle,
                         nullptr, 
                         newLeft,
                         newTop,
                         0, 0,    
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        
        HANDLE GetHandleFromObjectName(const WCHAR* objectName)
        {
            winrt::com_ptr<ID3D12Device> d3d12Device;
            HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, winrt::guid_of<ID3D12Device>(), d3d12Device.put_void());
            if (FAILED(hr)) {
                return nullptr;
            }

            HANDLE sharedHandle = nullptr;
            d3d12Device->OpenSharedHandleByName(objectName, GENERIC_ALL, &sharedHandle);
            
            return sharedHandle;
        }

        std::wstring GetProcessNameByPid(DWORD processId)
        {
            if (processId == 0) {
                return L"";
            }
            
            HANDLE processHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
            if (processHandle == NULL) {
                return L""; 
            }
            wil::unique_handle wilProcessHandle(processHandle);

            WCHAR imagePath[MAX_PATH];
            DWORD pathSize = MAX_PATH;
            if (QueryFullProcessImageNameW(wilProcessHandle.get(), 0, imagePath, &pathSize))
            {
                return std::wstring(imagePath);
            }
            
            return L"";
        }
    }
}