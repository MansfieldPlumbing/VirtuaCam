// =============================================================================
// Broker.h  --  Composites producer frames into the camera's frame
// =============================================================================
// The broker runs on its own thread and is event driven:
//
//   * a producer finishing a frame signals its shared fence; the broker has
//     an event armed on that fence (ID3D11Fence::SetEventOnCompletion), so
//     it wakes exactly when there is something new to draw;
//   * a producer process exiting signals its process handle;
//   * layout / output changes from the UI signal a wake event.
//
// Rendering is capped at the output frame rate with a high-resolution
// waitable timer, and skipped entirely when nothing changed.  Once a second
// the current frame is re-announced so the camera can tell a quiet broker
// from a dead one.  There is no polling loop and no per-frame process scan.
//
// Output: the shared "broker" manifest (see SharedFrame.h), which
// VirtuaCamSource.dll reads inside the Camera Frame Server.
// =============================================================================

#pragma once

#include "Compositor.h"
#include "SharedFrame.h"

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

enum class LayoutMode { Single, Grid };

// Source selection, by producer process id (0 = empty).
struct BrokerLayout {
    LayoutMode mode = LayoutMode::Single;
    DWORD main = 0;
    DWORD pip[4] = {};   // top-left, top-right, bottom-left, bottom-right
};

struct BrokerStatus {
    UINT wanted = 0;     // sources the layout asks for
    UINT live = 0;       // sources currently delivering frames
    UINT width = 0;
    UINT height = 0;
    UINT fps = 0;
};

class Broker {
public:
    Broker();
    Broker(const Broker&) = delete;
    Broker& operator=(const Broker&) = delete;
    ~Broker();

    // notifyWindow receives notifyMessage (at most one outstanding) whenever a
    // new frame is published or the status changes; call FrameConsumed() when
    // handling it.
    HRESULT Start(UINT width, UINT height, UINT fps, HWND notifyWindow, UINT notifyMessage);
    void Stop();

    void SetLayout(const BrokerLayout& layout);
    void SetOutput(UINT width, UINT height, UINT fps);
    BrokerStatus Status() const;
    void FrameConsumed() { m_notifyPending = false; }
    // Frame notifications are only wanted while something displays the output
    // (preview window, open menu); status changes are always reported.
    void SetFrameNotifications(bool enabled) { m_frameNotifications = enabled; }

    ID3D11Device* Device() const { return m_device.get(); }
    LUID Adapter() const { return m_adapter; }

    // Runs `draw` with the current output while the broker cannot render, so
    // the UI never observes a half-composited frame.
    void WithOutput(const std::function<void(ID3D11ShaderResourceView* view, UINT width, UINT height)>& draw);

private:
    struct Source;

    void Run();
    HRESULT CreateOutput(UINT width, UINT height);
    void SyncSources(ULONGLONG now, ULONGLONG& nextRetry);
    void Render();
    void Heartbeat();
    void Notify();

    wil::com_ptr_nothrow<ID3D11Device> m_device;
    wil::com_ptr_nothrow<ID3D11DeviceContext4> m_context;
    LUID m_adapter{};
    Compositor m_compositor;

    // Output (touched only by the broker thread, or under m_renderLock).
    wil::critical_section m_renderLock;
    Ipc::FramePublisher m_publisher;
    wil::com_ptr_nothrow<ID3D11RenderTargetView> m_outputTarget;
    wil::com_ptr_nothrow<ID3D11ShaderResourceView> m_outputView;
    wil::com_ptr_nothrow<ID3D11ShaderResourceView> m_noSignalView;
    UINT m_generation = 0;

    // Broker-thread state.
    std::vector<std::unique_ptr<Source>> m_sources;
    BrokerLayout m_activeLayout;
    std::vector<DWORD> m_gridPids;
    ULONGLONG m_nextGridScan = 0;
    ULONGLONG m_lastPublish = 0;
    LONGLONG m_frameInterval = 0;   // QPC ticks
    LONGLONG m_lastRender = 0;      // QPC ticks
    bool m_dirty = true;

    // Requests from the UI thread.
    mutable wil::critical_section m_controlLock;
    BrokerLayout m_layout;
    UINT m_width = 0, m_height = 0, m_fps = 0;
    bool m_layoutChanged = false;
    bool m_outputChanged = false;
    bool m_stopping = false;
    BrokerStatus m_status;

    wil::unique_event_nothrow m_wake;
    wil::unique_handle m_timer;
    std::thread m_thread;

    HWND m_notifyWindow = nullptr;
    UINT m_notifyMessage = 0;
    std::atomic<bool> m_notifyPending{ false };
    std::atomic<bool> m_frameNotifications{ false };
};
