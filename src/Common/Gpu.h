// =============================================================================
// Gpu.h  --  Small Direct3D 11 helpers shared by the app and the camera DLL
// =============================================================================

#pragma once

#include <windows.h>
#include <d3d11_4.h>
#include <wil/com.h>

namespace Gpu {

// Hardware device with BGRA support and multithread protection enabled (the
// broker thread and the UI thread share one immediate context).
HRESULT CreateDevice(ID3D11Device** device, UINT extraFlags = 0);

// Compiles an HLSL entry point with d3dcompiler_47 (loaded on demand).
HRESULT CompileShader(const char* source, const char* entry, const char* target, ID3DBlob** blob);

HRESULT CreateVertexShader(ID3D11Device* device, const char* source, const char* entry, ID3D11VertexShader** shader);
HRESULT CreatePixelShader(ID3D11Device* device, const char* source, const char* entry, ID3D11PixelShader** shader);

HRESULT CreateLinearClampSampler(ID3D11Device* device, ID3D11SamplerState** sampler);

// Static BGRA frame: dark radial gradient with the "NO SIGNAL" wordmark.
// Built once on the CPU; showing it is a single copy or draw.
HRESULT CreateNoSignalTexture(ID3D11Device* device, UINT width, UINT height, ID3D11Texture2D** texture);

// Largest rectangle with the source aspect ratio that fits inside dst, centred.
struct RectF { float x, y, w, h; };
RectF FitRect(const RectF& dst, UINT srcWidth, UINT srcHeight);

} // namespace Gpu
