// =============================================================================
// CameraIds.h  --  Identity of the VirtuaCam media source
// =============================================================================

#pragma once

#include <guiddef.h>

// COM class of the virtual camera media source in VirtuaCamSource.dll.
inline constexpr GUID CLSID_VirtuaCamSource =
    { 0xf2445826, 0xdf67, 0x4494, { 0x82, 0x9a, 0xff, 0x72, 0xf8, 0x4d, 0xb1, 0xaa } };
inline constexpr wchar_t kVirtuaCamSourceClsidString[] = L"{F2445826-DF67-4494-829A-FF72F84DB1AA}";

inline constexpr wchar_t kVirtuaCamFriendlyName[] = L"VirtuaCam";
inline constexpr wchar_t kVirtuaCamSourceDll[] = L"VirtuaCamSource.dll";
