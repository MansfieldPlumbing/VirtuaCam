// =============================================================================
// Whiteboard.cpp  --  A drawable canvas that is also a camera source
// =============================================================================
// Demonstrates what the shared-frame pipeline is for: any window can feed the
// camera.  Strokes are drawn with Direct2D straight into the shared 1080p
// texture and published only when the ink changes; the window shows the same
// texture through a DirectComposition presenter.  Put it in the main slot
// and your webcam in a picture-in-picture corner, and you can sketch while
// you talk.
//
//   drag            draw            1-6        colour (6 = eraser)
//   right-click     clear           [ / ]      thinner / thicker
//   Ctrl+Z          undo
// =============================================================================

#include "Producers.h"
#include "Gpu.h"
#include "Presenter.h"
#include "SharedFrame.h"

#include <d2d1_1.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <vector>

namespace {

constexpr UINT kCanvasWidth = 1920;
constexpr UINT kCanvasHeight = 1080;
constexpr wchar_t kWindowClass[] = L"VirtuaCamWhiteboard";
constexpr wchar_t kTitle[] = L"VirtuaCam Whiteboard  —  drag to draw · right-click clears · 1-6 colours · [ ] size · Ctrl+Z undo";

const D2D1_COLOR_F kPalette[] = {
    { 0.10f, 0.10f, 0.12f, 1 },   // ink
    { 0.86f, 0.20f, 0.18f, 1 },   // red
    { 0.16f, 0.42f, 0.86f, 1 },   // blue
    { 0.13f, 0.60f, 0.33f, 1 },   // green
    { 0.95f, 0.55f, 0.10f, 1 },   // orange
    { 1.00f, 1.00f, 1.00f, 1 },   // eraser (paper)
};

struct Stroke {
    D2D1_COLOR_F color;
    float width;
    std::vector<D2D1_POINT_2F> points;
};

class Whiteboard {
public:
    HRESULT Create(HINSTANCE instance);

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    D2D1_POINT_2F ToCanvas(LPARAM lParam) const;
    void DrawSegment(const Stroke& stroke, size_t from);
    void Redraw();
    void Commit();   // publish to the camera and refresh the window

    HWND m_hwnd = nullptr;
    wil::com_ptr_nothrow<ID3D11Device> m_device;
    wil::com_ptr_nothrow<ID3D11DeviceContext> m_context;
    wil::com_ptr_nothrow<ID2D1Factory1> m_factory;
    wil::com_ptr_nothrow<ID2D1DeviceContext> m_d2d;
    wil::com_ptr_nothrow<ID2D1Bitmap1> m_target;
    wil::com_ptr_nothrow<ID2D1SolidColorBrush> m_brush;
    wil::com_ptr_nothrow<ID2D1StrokeStyle> m_round;
    wil::com_ptr_nothrow<ID3D11ShaderResourceView> m_view;
    Ipc::FramePublisher m_publisher;
    Presenter m_presenter;

    std::vector<Stroke> m_strokes;
    bool m_drawing = false;
    size_t m_color = 0;
    float m_width = 6.0f;
};

HRESULT Whiteboard::Create(HINSTANCE instance)
{
    RETURN_IF_FAILED(Gpu::CreateDevice(&m_device));
    m_device->GetImmediateContext(&m_context);
    RETURN_IF_FAILED(m_publisher.OpenForProcess(m_device.get(), kCanvasWidth, kCanvasHeight,
                                                DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_RENDER_TARGET));
    RETURN_IF_FAILED(m_device->CreateShaderResourceView(m_publisher.Texture(), nullptr, &m_view));

    RETURN_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&m_factory)));
    wil::com_ptr_nothrow<IDXGIDevice> dxgi;
    RETURN_IF_FAILED(m_device->QueryInterface(IID_PPV_ARGS(&dxgi)));
    wil::com_ptr_nothrow<ID2D1Device> d2dDevice;
    RETURN_IF_FAILED(m_factory->CreateDevice(dxgi.get(), &d2dDevice));
    RETURN_IF_FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_d2d));

    wil::com_ptr_nothrow<IDXGISurface> surface;
    RETURN_IF_FAILED(m_publisher.Texture()->QueryInterface(IID_PPV_ARGS(&surface)));
    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    RETURN_IF_FAILED(m_d2d->CreateBitmapFromDxgiSurface(surface.get(), &properties, &m_target));
    m_d2d->SetTarget(m_target.get());
    m_d2d->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    RETURN_IF_FAILED(m_d2d->CreateSolidColorBrush(kPalette[0], &m_brush));
    const D2D1_STROKE_STYLE_PROPERTIES style = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    RETURN_IF_FAILED(m_factory->CreateStrokeStyle(style, nullptr, 0, &m_round));

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    const UINT dpi = GetDpiForSystem();
    RECT frame{ 0, 0, MulDiv(1024, dpi, 96), MulDiv(600, dpi, 96) };
    AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    m_hwnd = CreateWindowExW(0, kWindowClass, kTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             frame.right - frame.left, frame.bottom - frame.top, nullptr, nullptr, instance, this);
    RETURN_LAST_ERROR_IF_NULL(m_hwnd);

    const BOOL dark = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_TABBEDWINDOW;   // Mica Alt
    DwmSetWindowAttribute(m_hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    const MARGINS margins{ -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(m_hwnd, &margins);
    m_presenter.Attach(m_hwnd, false);

    Redraw();
    ShowWindow(m_hwnd, SW_SHOWNORMAL);
    return S_OK;
}

D2D1_POINT_2F Whiteboard::ToCanvas(LPARAM lParam) const
{
    const Gpu::RectF video = m_presenter.VideoRect();
    const float x = (GET_X_LPARAM(lParam) - video.x) / std::max(1.0f, video.w) * kCanvasWidth;
    const float y = (GET_Y_LPARAM(lParam) - video.y) / std::max(1.0f, video.h) * kCanvasHeight;
    return { x, y };
}

void Whiteboard::DrawSegment(const Stroke& stroke, size_t from)
{
    m_brush->SetColor(stroke.color);
    if (stroke.points.size() == 1) {
        const D2D1_ELLIPSE dot{ stroke.points[0], stroke.width * 0.5f, stroke.width * 0.5f };
        m_d2d->FillEllipse(dot, m_brush.get());
        return;
    }
    for (size_t i = std::max<size_t>(from, 1); i < stroke.points.size(); i++)
        m_d2d->DrawLine(stroke.points[i - 1], stroke.points[i], m_brush.get(), stroke.width, m_round.get());
}

void Whiteboard::Redraw()
{
    m_d2d->BeginDraw();
    m_d2d->Clear(kPalette[5]);
    for (const auto& stroke : m_strokes)
        DrawSegment(stroke, 0);
    m_d2d->EndDraw();
    Commit();
}

void Whiteboard::Commit()
{
    m_publisher.Publish(m_context.get());
    RECT client;
    GetClientRect(m_hwnd, &client);
    const int inset = MulDiv(12, GetDpiForWindow(m_hwnd), 96);
    InflateRect(&client, -inset, -inset);
    if (client.right > client.left && client.bottom > client.top)
        m_presenter.Present(m_view.get(), kCanvasWidth, kCanvasHeight, client, (float)inset * 0.75f);
}

LRESULT CALLBACK Whiteboard::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        auto* self = static_cast<Whiteboard*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    }
    auto* self = reinterpret_cast<Whiteboard*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT Whiteboard::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_LBUTTONDOWN:
        SetCapture(m_hwnd);
        m_drawing = true;
        m_strokes.push_back({ kPalette[m_color], m_color == 5 ? m_width * 4 : m_width, { ToCanvas(lParam) } });
        m_d2d->BeginDraw();
        DrawSegment(m_strokes.back(), 0);
        m_d2d->EndDraw();
        Commit();
        return 0;
    case WM_MOUSEMOVE:
        if (m_drawing) {
            Stroke& stroke = m_strokes.back();
            stroke.points.push_back(ToCanvas(lParam));
            m_d2d->BeginDraw();
            DrawSegment(stroke, stroke.points.size() - 1);
            m_d2d->EndDraw();
            Commit();
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (m_drawing && GetCapture() == m_hwnd)
            ReleaseCapture();
        m_drawing = false;
        return 0;
    case WM_RBUTTONUP:
        m_strokes.clear();
        Redraw();
        return 0;
    case WM_KEYDOWN:
        if (wParam >= '1' && wParam <= '6') {
            m_color = wParam - '1';
        } else if (wParam == VK_OEM_4) {
            m_width = std::max(2.0f, m_width - 2.0f);
        } else if (wParam == VK_OEM_6) {
            m_width = std::min(40.0f, m_width + 2.0f);
        } else if (wParam == 'Z' && (GetKeyState(VK_CONTROL) & 0x8000) && !m_strokes.empty()) {
            m_strokes.pop_back();
            Redraw();
        }
        return 0;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
            Commit();
        return 0;
    case WM_DPICHANGED: {
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(m_hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DESTROY:
        m_presenter.Detach();
        PostQuitMessage(ProducerExitOk);
        return 0;
    }
    return DefWindowProcW(m_hwnd, message, wParam, lParam);
}

} // namespace

int RunWhiteboardProducer(HINSTANCE instance)
{
    Whiteboard board;
    if (FAILED(board.Create(instance)))
        return ProducerExitStartFailed;
    return RunMessageLoop();
}
