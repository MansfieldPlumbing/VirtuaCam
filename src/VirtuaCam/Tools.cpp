// =============================================================================
// Tools.cpp  --  Shared utilities
// =============================================================================
// Miscellaneous helpers used across all VirtuaCam modules:
//   - Narrow/wide string conversion
//   - GUID formatting (for error messages and registry paths)
//   - Window centering (multi-monitor aware)
//   - HSL->RGB colour conversion (used by the UI)
//   - Software RGB32 -> NV12 colour-space conversion (BT.601, limited range)
//   - Registry read/write wrappers
//   - Cross-process D3D11 shared-handle lookup via D3D12
// =============================================================================

#include "pch.h"
#include "Tools.h"
#include <d3d12.h>
#include <algorithm>
#include <cstdint>

// ---------------------------------------------------------------------------
// String conversion helpers
// ---------------------------------------------------------------------------

std::string to_string(const std::wstring& ws)
{
    if (ws.empty())
        return std::string();

    auto wsize = (int)ws.size();
    auto ssize = WideCharToMultiByte(CP_THREAD_ACP, 0, ws.data(), wsize, nullptr, 0, nullptr, nullptr);
    if (!ssize)
        return std::string();

    std::string s;
    s.resize(ssize);
    ssize = WideCharToMultiByte(CP_THREAD_ACP, 0, ws.data(), wsize, &s[0], ssize, nullptr, nullptr);
    if (!ssize)
        return std::string();

    return s;
}

std::wstring to_wstring(const std::string& s)
{
    if (s.empty())
        return std::wstring();

    auto ssize = (int)s.size();
    auto wsize = MultiByteToWideChar(CP_THREAD_ACP, 0, s.data(), ssize, nullptr, 0);
    if (!wsize)
        return std::wstring();

    std::wstring ws;
    ws.resize(wsize);
    wsize = MultiByteToWideChar(CP_THREAD_ACP, 0, s.data(), ssize, &ws[0], wsize);
    if (!wsize)
        return std::wstring();

    return ws;
}

// ---------------------------------------------------------------------------
// GUID formatting
// ---------------------------------------------------------------------------

const std::string GUID_ToStringA(const GUID& guid) { return to_string(GUID_ToStringW(guid)); }
const std::wstring GUID_ToStringW(const GUID& guid)
{
    // Standard "{xxxxxxxx-xxxx-...}" representation.
    wchar_t name[64];
    std::ignore = StringFromGUID2(guid, name, _countof(name));
    return name;
}

// ---------------------------------------------------------------------------
// Window centering (multi-monitor aware)
// ---------------------------------------------------------------------------

void CenterWindow(HWND hwnd, bool useCursorPos)
{
    if (!IsWindow(hwnd))
        return;

    RECT rc{};
    GetWindowRect(hwnd, &rc);
    auto width = rc.right - rc.left;
    auto height = rc.bottom - rc.top;

    if (useCursorPos)
    {
        // Centre on whichever monitor the mouse cursor is on.
        POINT pt{};
        if (GetCursorPos(&pt))
        {
            auto monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            MONITORINFOEX  mi{};
            mi.cbSize = sizeof(MONITORINFOEX);
            if (GetMonitorInfo(monitor, &mi))
            {
                SetWindowPos(hwnd, NULL, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - width) / 2, mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - height) / 2, 0, 0, SWP_NOREDRAW | SWP_NOSIZE | SWP_NOZORDER);
                return;
            }
        }
    }

    // Fallback: centre on the primary monitor.
    SetWindowPos(hwnd, NULL, (GetSystemMetrics(SM_CXSCREEN) - width) / 2, (GetSystemMetrics(SM_CYSCREEN) - height) / 2, 0, 0, SWP_NOREDRAW | SWP_NOSIZE | SWP_NOZORDER);
}

// ---------------------------------------------------------------------------
// HSL -> RGB colour conversion
// ---------------------------------------------------------------------------
// Used by the UI to generate colours from hue/saturation/lightness values.
// Algorithm: standard two-step HSL decomposition into RGB primaries.

inline float HUE2RGB(const float p, const float q, float t)
{
    if (t < 0)
        t += 1;
    if (t > 1)
        t -= 1;
    if (t < 1 / 6.0f)
        return p + (q - p) * 6 * t;
    if (t < 1 / 2.0f)
        return q;
    if (t < 2 / 3.0f)
        return p + (q - p) * (2 / 3.0f - t) * 6;
    return p;
}

D2D1_COLOR_F HSL2RGB(const float h, const float s, const float l)
{
    D2D1_COLOR_F result;
    result.a = 1;

    if (!s)
    {
        // Achromatic (grey): all channels equal lightness.
        result.r = l;
        result.g = l;
        result.b = l;
    }
    else
    {
        auto q = l < 0.5f ? l * (1 + s) : l + s - l * s;
        auto p = 2 * l - q;
        result.r = HUE2RGB(p, q, h + 1 / 3.0f);
        result.g = HUE2RGB(p, q, h);
        result.b = HUE2RGB(p, q, h - 1 / 3.0f);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Registry helpers
// ---------------------------------------------------------------------------

const LSTATUS RegWriteKey(HKEY key, PCWSTR path, HKEY* outKey)
{
    *outKey = nullptr;
    return RegCreateKeyEx(key, path, 0, nullptr, 0, KEY_WRITE, nullptr, outKey, nullptr);
}

const LSTATUS RegWriteValue(HKEY key, PCWSTR name, const std::wstring& value)
{
    return RegSetValueEx(key, name, 0, REG_SZ, reinterpret_cast<BYTE const*>(value.c_str()), static_cast<uint32_t>((value.size() + 1) * sizeof(wchar_t)));
}

const LSTATUS RegWriteValue(HKEY key, PCWSTR name, DWORD value)
{
    return RegSetValueEx(key, name, 0, REG_DWORD, reinterpret_cast<BYTE const*>(&value), sizeof(value));
}

// ---------------------------------------------------------------------------
// Software YUV conversion  (BT.601, limited / "studio" range)
// ---------------------------------------------------------------------------
// These functions convert RGB pixels to the YCbCr colour space used by NV12.
//
// The integer coefficients are the standard BT.601 limited-range approximation:
//
//   Y  = ( 66*R + 129*G +  25*B + 128) >> 8  + 16
//   Cb = (-38*R -  74*G + 112*B + 128) >> 8  + 128
//   Cr = (112*R -  94*G -  18*B + 128) >> 8  + 128
//
// The +128 before the shift is a rounding bias (equivalent to 0.5 after the
// divide).  The +16 and +128 offsets are the limited-range foot levels for Y
// and chroma respectively (Y: [16,235], Cb/Cr: [16,240]).
//
// Reference: ITU-R BT.601-7, Section 2.5.3

static inline void RGB24ToYUY2(int r, int g, int b, BYTE* y, BYTE* u, BYTE* v)
{
    *y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
    *u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
    *v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
}

// Y-only variant (used for pixels that share Cb/Cr with a neighbour due to
// 4:2:0 chroma subsampling).
static inline void RGB24ToY(int r, int g, int b, BYTE* y)
{
    *y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
}

// Converts two horizontally adjacent pixels from two vertically adjacent rows
// (a 2x2 block) into NV12 format.  NV12 has a full-resolution Y plane followed
// by a half-resolution interleaved UV plane (4:2:0 chroma subsampling).
// Input:  rgb1 = top row (two BGRA pixels = 8 bytes)
//         rgb2 = bottom row (two BGRA pixels = 8 bytes)
//         Note: BGRA byte order, so channel indices are [0]=B, [1]=G, [2]=R, [3]=A.
// Output: y1/y2 = luma for top/bottom row pixels (2 bytes each)
//         uv    = interleaved Cb,Cr for the 2x2 block (2 bytes)
static inline void RGB32ToNV12(BYTE rgb1[8], BYTE rgb2[8], BYTE* y1, BYTE* y2, BYTE* uv)
{
    // Top-left pixel: compute Y + U/V (U/V represent the whole 2x2 block)
    RGB24ToYUY2(rgb1[2], rgb1[1], rgb1[0], y1, uv, uv + 1);
    // Top-right pixel: Y only (shares U/V with top-left)
    RGB24ToY(rgb1[6], rgb1[5], rgb1[4], y1 + 1);
    // Bottom-left pixel: Y only (shares U/V — chroma overwritten, but values are similar)
    RGB24ToYUY2(rgb2[2], rgb2[1], rgb2[0], y2, uv, uv + 1);
    // Bottom-right pixel: Y only
    RGB24ToY(rgb2[6], rgb2[5], rgb2[4], y2 + 1);
};

// Full-frame RGB32 -> NV12 conversion.
// Processes the image two rows at a time to correctly handle 4:2:0 chroma
// subsampling (each U/V sample covers a 2x2 pixel block).
// Output layout: Y plane [width x height bytes] then UV plane [width x height/2 bytes].
HRESULT RGB32ToNV12(BYTE* input, ULONG inputSize, LONG inputStride, UINT width, UINT height, BYTE* output, ULONG ouputSize, LONG outputStride)
{
    RETURN_HR_IF_NULL(E_INVALIDARG, input);
    RETURN_HR_IF_NULL(E_INVALIDARG, output);
    RETURN_HR_IF(E_UNEXPECTED, width * 4 * height > inputSize);
    RETURN_HR_IF(E_UNEXPECTED, width * 1.5 * height > ouputSize);

    for (DWORD h = 0; h < height - 1; h += 2)
    {
        auto rgb1     = h * inputStride + input;
        auto rgb2pRGB2 = (h + 1) * inputStride + input;
        auto y1       = h * outputStride + output;
        auto y2       = (h + 1) * outputStride + output;
        // UV plane starts immediately after the Y plane at row offset 'height'.
        auto uv       = (h / 2 + height) * outputStride + output;
        for (DWORD w = 0; w < width; w += 2)
        {
            RGB32ToNV12(rgb1, rgb2pRGB2, y1, y2, uv);
            rgb1     += 8;
            rgb2pRGB2 += 8;
            y1  += 2;
            y2  += 2;
            uv  += 2;
        }
    }
    return S_OK;
}

// ---------------------------------------------------------------------------
// Cross-process D3D11 shared-handle lookup
// ---------------------------------------------------------------------------
// D3D11 does not expose OpenSharedHandleByName; D3D12 does.
// We create a temporary D3D12 device solely to call OpenSharedHandleByName,
// then return the NT handle so the caller can open it with D3D11.
HANDLE GetHandleFromName(const WCHAR* name)
{
    wil::com_ptr_nothrow<ID3D12Device> d3d12Device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12Device))))
    {
        return NULL;
    }
    HANDLE handle = nullptr;
    d3d12Device->OpenSharedHandleByName(name, GENERIC_ALL, &handle);
    return handle;
}

// ---------------------------------------------------------------------------
// "No Signal" placeholder texture
// ---------------------------------------------------------------------------
// CPU-rasters a static placeholder frame once and uploads it as an immutable
// texture: a subtle dark radial-gradient background with a pre-rendered
// "NO SIGNAL" wordmark (Selawik, letter-spaced — see NoSignalMask.h) blended
// in the centre.  Displaying the placeholder is a single CopyResource per
// frame — no shaders, no animation, no per-frame CPU work.

#include "NoSignalMask.h"

namespace
{
    // Bilinear sample of the wordmark alpha mask at fractional coordinates.
    float SampleMask(float x, float y)
    {
        if (x < 0 || y < 0 || x > kNoSignalMaskWidth - 1.0f || y > kNoSignalMaskHeight - 1.0f)
            return 0.0f;
        const UINT x0 = (UINT)x, y0 = (UINT)y;
        const UINT x1 = std::min(x0 + 1, kNoSignalMaskWidth - 1);
        const UINT y1 = std::min(y0 + 1, kNoSignalMaskHeight - 1);
        const float fx = x - x0, fy = y - y0;
        const float top = kNoSignalMask[y0 * kNoSignalMaskWidth + x0] * (1 - fx) + kNoSignalMask[y0 * kNoSignalMaskWidth + x1] * fx;
        const float bot = kNoSignalMask[y1 * kNoSignalMaskWidth + x0] * (1 - fx) + kNoSignalMask[y1 * kNoSignalMaskWidth + x1] * fx;
        return (top * (1 - fy) + bot * fy) / 255.0f;
    }
}

HRESULT CreateNoSignalTexture(ID3D11Device* device, UINT width, UINT height, ID3D11Texture2D** outTexture)
{
    RETURN_HR_IF_NULL(E_POINTER, device);
    RETURN_HR_IF_NULL(E_POINTER, outTexture);
    *outTexture = nullptr;
    RETURN_HR_IF(E_INVALIDARG, !width || !height);

    // Background: subtle radial gradient, slightly cool dark grey falling off
    // to near-black at the corners.  Rastered once, so per-pixel cost is free.
    std::vector<uint32_t> pixels((size_t)width * height);
    {
        constexpr float centerR = 24.0f, centerG = 26.0f, centerB = 30.0f;
        constexpr float edgeR   =  7.0f, edgeG   =  8.0f, edgeB   = 10.0f;
        const float cx = width * 0.5f, cy = height * 0.5f;
        const float invMaxDist = 1.0f / std::sqrt(cx * cx + cy * cy);
        for (UINT y = 0; y < height; y++)
        {
            const float dy = (float)y - cy;
            for (UINT x = 0; x < width; x++)
            {
                const float dx = (float)x - cx;
                float t = std::sqrt(dx * dx + dy * dy) * invMaxDist;
                t = t * t * (3.0f - 2.0f * t);  // smoothstep for a soft falloff
                const uint32_t r = (uint32_t)(centerR + (edgeR - centerR) * t);
                const uint32_t g = (uint32_t)(centerG + (edgeG - centerG) * t);
                const uint32_t b = (uint32_t)(centerB + (edgeB - centerB) * t);
                pixels[(size_t)y * width + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
    }

    // Wordmark: ~1/12th of the frame height, never wider than half the frame,
    // alpha-blended over the gradient with bilinear scaling.
    {
        float scale = (height / 12.0f) / kNoSignalMaskHeight;
        if (kNoSignalMaskWidth * scale > width * 0.5f)
            scale = (width * 0.5f) / kNoSignalMaskWidth;
        const UINT outW = std::max(1u, (UINT)(kNoSignalMaskWidth  * scale));
        const UINT outH = std::max(1u, (UINT)(kNoSignalMaskHeight * scale));
        const UINT originX = (width  > outW) ? (width  - outW) / 2 : 0;
        const UINT originY = (height > outH) ? (height - outH) / 2 : 0;
        constexpr float textR = 0x8A, textG = 0x8F, textB = 0x98;  // soft grey, slightly cool

        for (UINT y = 0; y < outH && originY + y < height; y++)
            for (UINT x = 0; x < outW && originX + x < width; x++)
            {
                const float a = SampleMask(x / scale, y / scale);
                if (a <= 0.0f)
                    continue;
                uint32_t& dst = pixels[(size_t)(originY + y) * width + (originX + x)];
                const float bgR = (float)((dst >> 16) & 0xFF);
                const float bgG = (float)((dst >>  8) & 0xFF);
                const float bgB = (float)( dst        & 0xFF);
                const uint32_t r = (uint32_t)(bgR + (textR - bgR) * a);
                const uint32_t g = (uint32_t)(bgG + (textG - bgG) * a);
                const uint32_t b = (uint32_t)(bgB + (textB - bgB) * a);
                dst = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width            = width;
    desc.Height           = height;
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = { pixels.data(), width * 4, 0 };
    RETURN_IF_FAILED(device->CreateTexture2D(&desc, &init, outTexture));
    return S_OK;
}
