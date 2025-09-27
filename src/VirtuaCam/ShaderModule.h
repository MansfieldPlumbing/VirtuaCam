#pragma once
#include <windows.h>
#include <d3d11_4.h>
#include "wil/com.h" // Use WIL for smart pointers
#include <chrono>    // Added for timekeeping

class ShaderModule {
public:
    ShaderModule();
    ~ShaderModule();

    HRESULT Initialize(wil::com_ptr_nothrow<ID3D11Device> device);
    void Shutdown();
    void Render(wil::com_ptr_nothrow<ID3D11RenderTargetView> rtv, UINT width, UINT height);

private:
    wil::com_ptr_nothrow<ID3D11Device> m_device;
    wil::com_ptr_nothrow<ID3D11DeviceContext> m_context;
    wil::com_ptr_nothrow<ID3D11VertexShader> m_vs;
    wil::com_ptr_nothrow<ID3D11PixelShader> m_ps;
    wil::com_ptr_nothrow<ID3D11SamplerState> m_sampler;
    wil::com_ptr_nothrow<ID3D11Buffer> m_constantBuffer;     // New: For sending data to the shader
    std::chrono::steady_clock::time_point m_startTime;      // New: For animation timer

    // This vertex shader generates a full-screen triangle. It works with the new pixel shader
    // which takes SV_Position as an input.
    const char* g_OffModeVertexShader = R"(
struct VS_OUTPUT {
    float4 Pos : SV_POSITION;
};
VS_OUTPUT main(uint id : SV_VertexID) {
    VS_OUTPUT output;
    // Generates a triangle covering the entire screen
    float2 uv = float2((id << 1) & 2, id & 2);
    output.Pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1);
    return output;
}
)";

    // New: The pixel shader from PC.hlsl is now embedded here.
    const char* g_OffModePixelShader = R"shader(
// Retro_SNES_PC_Screen.hlsl (Pixel Shader, entry: main, target: ps_5_0)
// Emulates a late 90s CRT with large, high-density pixel font text.

cbuffer Constants : register(b0) {
    float4 bar_rect;      // unused
    float2 resolution;    // (width, height)
    float  time;          // seconds
    float  pad;
}

// ==============================
//        T U N A B L E S
// ==============================

// --- Math Constants ---
static const float PI = 3.14159265f;

// --- Screen & Text Colors ---
static const float3 SCREEN_COLOR = float3(0.05, 0.1, 0.85);  // Deep Blue
static const float3 TEXT_COLOR   = float3(0.15, 1.0, 0.25); // Bright Neon Green

// --- Text Layout ---
static const float2 TEXT_POSITION = float2(0.85, 0.12); // Top-right corner (UV space)
static const float  TEXT_SCALE    = 8.0;                // Controls the overall size of the text

// --- CRT Effects ---
static const float SCANLINE_INTENSITY      = 0.3;   // How dark scanlines are (0=off)
static const float SCANLINE_SPEED          = 0.1;   // Slow vertical scroll
static const float VIGNETTE_STRENGTH       = 0.4;   // Darkening at screen edges
static const float CHROMATIC_ABERRATION    = 0.002; // Color bleed at edges
static const float FLICKER_INTENSITY       = 0.03;  // Subtle brightness flicker

// ==============================
//      16x16 FONT BITMAP ('P', 'C')
// ==============================

// Samples a 16x16 pixel font for higher resolution text.
// 1 = 'P', 2 = 'C'. Returns 1.0 if the pixel is on, 0.0 if off.
float SampleFont16x16(int char_id, float2 char_uv)
{
    // Discard any UV coordinates outside the character's 0-1 box
    if (char_uv.x < 0.0 || char_uv.x > 1.0 || char_uv.y < 0.0 || char_uv.y > 1.0) {
        return 0.0;
    }

    // Snap UVs to a 16x16 grid
    int x = (int)(char_uv.x * 16.0);
    int y = (int)(char_uv.y * 16.0);

    if (char_id == 1) // Character 'P' (16x16)
    {
        const int font_p[16][16] = {
            {0,0,1,1,1,1,1,1,0,0,0,0,0,0,0,0},
            {0,0,1,1,1,1,1,1,1,0,0,0,0,0,0,0},
            {0,0,1,1,0,0,0,1,1,0,0,0,0,0,0,0},
            {0,0,1,1,0,0,0,0,1,1,0,0,0,0,0,0},
            {0,0,1,1,0,0,0,0,1,1,0,0,0,0,0,0},
            {0,0,1,1,0,0,0,1,1,0,0,0,0,0,0,0},
            {0,0,1,1,1,1,1,1,1,0,0,0,0,0,0,0},
            {0,0,1,1,1,1,1,1,0,0,0,0,0,0,0,0},
            {0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0},
            {0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0},
            {0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0},
            {0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0},
            {0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0},
            {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
            {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
            {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
        };
        return (y < 16 && x < 16) ? (float)font_p[y][x] : 0.0;
    }
    if (char_id == 2) // Character 'C' (16x16)
    {
        const int font_c[16][16] = {
            {0,0,0,0,1,1,1,1,1,0,0,0,0,0,0,0},
            {0,0,0,1,1,0,0,0,1,1,0,0,0,0,0,0},
            {0,0,1,1,0,0,0,0,0,1,0,0,0,0,0,0},
            {0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0},
            {0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0},
            {0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0},
            {0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0},
            {0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0},
            {0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0},
            {0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0},
            {0,0,1,1,0,0,0,0,0,1,0,0,0,0,0,0},
            {0,0,0,1,1,0,0,0,1,1,0,0,0,0,0,0},
            {0,0,0,0,1,1,1,1,1,0,0,0,0,0,0,0},
            {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
            {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
            {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
        };
        return (y < 16 && x < 16) ? (float)font_c[y][x] : 0.0;
    }
    return 0.0;
}


// ==============================
//        I M P L E M E N T
// ==============================

float4 main(float4 svpos : SV_Position) : SV_Target
{
    // 1. Calculate UV coordinates [0,1], correcting for aspect ratio
    float2 aspect_ratio = float2(resolution.x / resolution.y, 1.0);
    float2 uv = svpos.xy / resolution.xy;

    // 2. Sample the font for each channel to create Chromatic Aberration
    // FIXED: Corrected the typo in the variable name below
    float ca = CHROMATIC_ABERRATION;
    
    // Define the UV space for each character, with adjusted spacing for the new font.
    float2 p_uv = (uv - TEXT_POSITION) * aspect_ratio * TEXT_SCALE;
    float2 c_uv = (uv - TEXT_POSITION - float2(0.055, 0)) * aspect_ratio * TEXT_SCALE;
    
    // Sample Red channel slightly to the left
    float text_r = SampleFont16x16(1, p_uv - float2(ca, 0)) + SampleFont16x16(2, c_uv - float2(ca, 0));
    // Sample Green channel at the center
    float text_g = SampleFont16x16(1, p_uv) + SampleFont16x16(2, c_uv);
    // Sample Blue channel slightly to the right
    float text_b = SampleFont16x16(1, p_uv + float2(ca, 0)) + SampleFont16x16(2, c_uv + float2(ca, 0));
    
    // Combine the channels into a single color mask
    float3 text_mask = saturate(float3(text_r, text_g, text_b));

    // 3. Combine base screen color with the text color
    float3 color = lerp(SCREEN_COLOR, TEXT_COLOR, text_mask);

    // 4. Add Scanlines
    float scanline_phase = (uv.y + time * SCANLINE_SPEED) * resolution.y * 0.5;
    float scanline_effect = 1.0 - (sin(scanline_phase) * SCANLINE_INTENSITY);
    color *= scanline_effect;

    // 5. Add Vignette
    float2 centered_uv = (uv - 0.5) * 2.0;
    float vignette = 1.0 - dot(centered_uv, centered_uv) * VIGNETTE_STRENGTH;
    color *= vignette;
    
    // 6. Add subtle time-based flicker
    float noise = frac(sin(dot(uv + time, float2(12.9898, 78.233))) * 43758.5453);
    color *= (1.0 - noise * FLICKER_INTENSITY);

    return float4(color, 1.0);
}
)shader";
};