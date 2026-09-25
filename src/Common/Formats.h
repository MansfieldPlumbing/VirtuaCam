// =============================================================================
// Formats.h  --  Video modes offered by the virtual camera and the broker
// =============================================================================
// The camera advertises every (size x rate x subtype) combination below and
// scales the broker's output to whatever the consuming app negotiates, so the
// broker resolution and the camera resolution are independent.
// =============================================================================

#pragma once

#include <windows.h>

namespace Formats {

struct Size { UINT width, height; };
struct Rate { UINT numerator, denominator; };

// Ordered by preference: apps that take the first media type get 1080p.
inline constexpr Size kCameraSizes[] = {
    { 1920, 1080 }, { 1280, 720 }, { 3840, 2160 }, { 2560, 1440 },
    { 960, 540 },   { 640, 360 },  { 1440, 1080 }, { 640, 480 },
};

inline constexpr Rate kCameraRates[] = {
    { 30, 1 }, { 60, 1 }, { 24, 1 }, { 15, 1 },
};

// Broker (composite) output sizes selectable from the tray menu.
inline constexpr Size kOutputSizes[] = {
    { 1280, 720 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 },
};

inline constexpr UINT kOutputRates[] = { 30, 60 };

inline constexpr Size kDefaultOutputSize = { 1920, 1080 };
inline constexpr UINT kDefaultOutputRate = 60;

// 100-ns units per frame for a rational rate.
inline LONGLONG FrameDuration(UINT numerator, UINT denominator)
{
    return numerator ? (LONGLONG)(10000000ULL * denominator / numerator) : 333333;
}

} // namespace Formats
