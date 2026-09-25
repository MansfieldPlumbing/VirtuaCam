// =============================================================================
// Presenter.h  --  Shows a texture inside a window through DirectComposition
// =============================================================================
// The video is presented as a DirectComposition visual rather than painted
// into the window: a flip-model composition swap chain sized to the fitted
// video rectangle, positioned and corner-clipped by the visual tree.  DWM
// composites it over whatever the window paints (Mica, menu items), so the
// window never repaints for video and resizing never touches GDI.
//
// Present() is called when a new frame exists -- there is no render loop.
// =============================================================================

#pragma once

#include "Gpu.h"

#include <dcomp.h>
#include <dxgi1_2.h>
#include <wil/com.h>

class Presenter {
public:
    Presenter() = default;
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;
    ~Presenter() { Detach(); }

    // topmost: draw above the window's own GDI content (menus).
    void Attach(HWND hwnd, bool topmost) { m_hwnd = hwnd; m_topmost = topmost; }
    void Detach();
    bool IsAttached() const { return m_hwnd != nullptr; }

    // Draws `view` (source size sourceWidth x sourceHeight) aspect-fitted
    // into `bounds` (client pixels) with rounded corners, and presents it.
    HRESULT Present(ID3D11ShaderResourceView* view, UINT sourceWidth, UINT sourceHeight,
                    const RECT& bounds, float cornerRadius = 0.0f);

    // Where the video landed in client pixels (for mapping mouse input).
    Gpu::RectF VideoRect() const { return m_videoRect; }

private:
    HRESULT EnsureComposition(ID3D11Device* device);
    HRESULT EnsureSwapChain(UINT width, UINT height);

    HWND m_hwnd = nullptr;
    bool m_topmost = false;
    wil::com_ptr_nothrow<ID3D11Device> m_device;
    wil::com_ptr_nothrow<IDCompositionDevice> m_composition;
    wil::com_ptr_nothrow<IDCompositionTarget> m_target;
    wil::com_ptr_nothrow<IDCompositionVisual> m_visual;
    wil::com_ptr_nothrow<IDCompositionRectangleClip> m_clip;
    wil::com_ptr_nothrow<IDXGISwapChain1> m_swapChain;
    wil::com_ptr_nothrow<ID3D11RenderTargetView> m_backBuffer;
    wil::com_ptr_nothrow<ID3D11VertexShader> m_vertexShader;
    wil::com_ptr_nothrow<ID3D11PixelShader> m_pixelShader;
    wil::com_ptr_nothrow<ID3D11SamplerState> m_sampler;
    wil::com_ptr_nothrow<ID3D11Buffer> m_constants;
    UINT m_width = 0;
    UINT m_height = 0;
    Gpu::RectF m_videoRect{};
    float m_cornerRadius = -1.0f;
};
