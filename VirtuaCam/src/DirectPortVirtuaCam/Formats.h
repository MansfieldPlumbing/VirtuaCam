#pragma once
#include <vector>
#include <string>

struct Resolution {
    UINT width;
    UINT height;
    const wchar_t* name;
};

struct FrameRate {
    UINT numerator;
    UINT denominator;
    const wchar_t* name;
};

static const std::vector<Resolution> g_supportedResolutions = {
    // 16:9 Resolutions
    {1280, 720,  L"1280x720 (16:9 HD)"},
    {1920, 1080, L"1920x1080 (16:9 FullHD)"},
    {2560, 1440, L"2560x1440 (16:9 QHD)"},
    {3840, 2160, L"3840x2160 (16:9 4K UHD)"},
    {960, 540,   L"960x540 (16:9 qHD)"},
    {854, 480,   L"854x480 (16:9)"},
    {640, 360,   L"640x360 (16:9)"},

    // 4:3 Resolutions
    {640, 480,   L"640x480 (4:3 VGA)"},
    {800, 600,   L"800x600 (4:3 SVGA)"},
    {1024, 768,  L"1024x768 (4:3 XGA)"},
    {1280, 960,  L"1280x960 (4:3)"},
    {1600, 1200, L"1600x1200 (4:3 UXGA)"},

    // 16:10 Resolutions
    {1280, 800,  L"1280x800 (16:10 WXGA)"},
    {1920, 1200, L"1920x1200 (16:10 WUXGA)"},
    {2560, 1600, L"2560x1600 (16:10 WQXGA)"}
};

static const std::vector<FrameRate> g_supportedFrameRates = {
    {120, 1, L"120 FPS"},
    {60, 1, L"60 FPS"},
    {30, 1, L"30 FPS"},
    {30000, 1001, L"29.97 FPS (NTSC)"},
    {24, 1, L"24 FPS (Cinematic)"}
};