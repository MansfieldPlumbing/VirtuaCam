// =============================================================================
// Broker.cpp
// =============================================================================

#include "Broker.h"
#include "Discovery.h"
#include "Gpu.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr ULONGLONG kHeartbeatMs = 1000;     // re-announce the current frame
constexpr ULONGLONG kRetryMs = 250;          // producer started but manifest not up yet
constexpr ULONGLONG kGridScanMs = 2000;      // grid mode picks up new producers
constexpr size_t kMaxSources = 16;           // 2 wait handles each, well under 64

LONGLONG QpcNow()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

LONGLONG QpcFrequency()
{
    static const LONGLONG frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    return frequency;
}

std::wstring OutputName(const wchar_t* base, UINT generation)
{
    std::wstring name = base;
    if (generation)
        name += L"_" + std::to_wstring(generation);
    return name;
}

} // namespace

struct Broker::Source {
    DWORD pid = 0;
    Ipc::FrameSubscription frames;
    wil::unique_handle process;
    ULONGLONG nextAttempt = 0;
    bool drawnSmall = false;   // minified: the shader samples mips, so they must be current

    bool Live() const { return frames.IsOpen() && frames.LastCopiedFrame() > 0; }
};

// -----------------------------------------------------------------------------
// Lifetime
// -----------------------------------------------------------------------------

Broker::Broker() = default;

Broker::~Broker()
{
    Stop();
}

HRESULT Broker::Start(UINT width, UINT height, UINT fps, HWND notifyWindow, UINT notifyMessage)
{
    m_notifyWindow = notifyWindow;
    m_notifyMessage = notifyMessage;

    RETURN_IF_FAILED(Gpu::CreateDevice(&m_device));
    wil::com_ptr_nothrow<ID3D11DeviceContext> context;
    m_device->GetImmediateContext(&context);
    RETURN_IF_FAILED(context->QueryInterface(IID_PPV_ARGS(&m_context)));
    m_adapter = Ipc::AdapterLuidOf(m_device.get());
    RETURN_IF_FAILED(m_compositor.Initialize(m_device.get()));

    wil::com_ptr_nothrow<ID3D11Texture2D> noSignal;
    RETURN_IF_FAILED(Gpu::CreateNoSignalTexture(m_device.get(), 1920, 1080, &noSignal));
    RETURN_IF_FAILED(m_device->CreateShaderResourceView(noSignal.get(), nullptr, &m_noSignalView));

    m_width = width;
    m_height = height;
    m_fps = fps;
    RETURN_IF_FAILED(CreateOutput(width, height));
    m_frameInterval = QpcFrequency() / std::max(1u, fps);
    m_status.width = width;
    m_status.height = height;
    m_status.fps = fps;

    RETURN_IF_FAILED(m_wake.create(wil::EventOptions::None));
    m_timer.reset(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS));
    if (!m_timer)   // pre-1803 Windows: ordinary resolution
        m_timer.reset(CreateWaitableTimerW(nullptr, FALSE, nullptr));
    RETURN_LAST_ERROR_IF(!m_timer);

    m_thread = std::thread([this] { Run(); });
    return S_OK;
}

void Broker::Stop()
{
    if (m_thread.joinable()) {
        {
            auto lock = m_controlLock.lock();
            m_stopping = true;
        }
        m_wake.SetEvent();
        m_thread.join();
    }
    m_sources.clear();
    auto lock = m_renderLock.lock();
    m_publisher.Close();
    m_outputTarget.reset();
    m_outputView.reset();
}

void Broker::SetLayout(const BrokerLayout& layout)
{
    {
        auto lock = m_controlLock.lock();
        m_layout = layout;
        m_layoutChanged = true;
    }
    m_wake.SetEvent();
}

void Broker::SetOutput(UINT width, UINT height, UINT fps)
{
    {
        auto lock = m_controlLock.lock();
        m_width = width;
        m_height = height;
        m_fps = fps;
        m_outputChanged = true;
    }
    m_wake.SetEvent();
}

BrokerStatus Broker::Status() const
{
    auto lock = m_controlLock.lock();
    return m_status;
}

void Broker::WithOutput(const std::function<void(ID3D11ShaderResourceView*, UINT, UINT)>& draw)
{
    auto lock = m_renderLock.lock();
    if (m_outputView)
        draw(m_outputView.get(), m_publisher.Width(), m_publisher.Height());
}

void Broker::Notify()
{
    if (m_notifyWindow && !m_notifyPending.exchange(true))
        PostMessageW(m_notifyWindow, m_notifyMessage, 0, 0);
}

// -----------------------------------------------------------------------------
// Output
// -----------------------------------------------------------------------------

HRESULT Broker::CreateOutput(UINT width, UINT height)
{
    auto lock = m_renderLock.lock();
    m_outputTarget.reset();
    m_outputView.reset();

    // A resized output gets new object names: the camera may still hold the
    // old ones open, and named objects cannot be re-created while in use.
    const std::wstring texture = OutputName(L"Local\\VirtuaCast_Broker_Texture", m_generation);
    const std::wstring fence = OutputName(L"Local\\VirtuaCast_Broker_Fence", m_generation);
    m_generation++;

    if (m_publisher.IsOpen())
        RETURN_IF_FAILED(m_publisher.Recreate(width, height, texture, fence));
    else
        RETURN_IF_FAILED(m_publisher.Open(m_device.get(), width, height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                          Ipc::kBrokerManifestName, texture, fence, D3D11_BIND_RENDER_TARGET));
    RETURN_IF_FAILED(m_device->CreateRenderTargetView(m_publisher.Texture(), nullptr, &m_outputTarget));
    RETURN_IF_FAILED(m_device->CreateShaderResourceView(m_publisher.Texture(), nullptr, &m_outputView));
    return S_OK;
}

// -----------------------------------------------------------------------------
// Broker thread
// -----------------------------------------------------------------------------

void Broker::Run()
{
    SetThreadDescription(GetCurrentThread(), L"VirtuaCam broker");

    std::vector<HANDLE> handles;
    std::vector<std::pair<Source*, bool>> handleOwners;   // (source, isProcessHandle)

    for (;;) {
        // 1. Apply requests from the UI.
        {
            auto lock = m_controlLock.lock();
            if (m_stopping)
                break;
            if (m_layoutChanged) {
                m_activeLayout = m_layout;
                m_layoutChanged = false;
                m_nextGridScan = 0;
                m_dirty = true;
            }
            if (m_outputChanged) {
                m_outputChanged = false;
                if (m_width != m_publisher.Width() || m_height != m_publisher.Height())
                    CreateOutput(m_width, m_height);
                m_status.width = m_publisher.Width();
                m_status.height = m_publisher.Height();
                m_frameInterval = QpcFrequency() / std::max(1u, m_fps);
                m_status.fps = m_fps;
                m_dirty = true;
            }
        }

        // 2. Connect to / drop producers to match the layout.
        const ULONGLONG now = GetTickCount64();
        ULONGLONG nextDeadline = m_lastPublish + kHeartbeatMs;
        SyncSources(now, nextDeadline);

        // 3. Draw if something changed and the frame-rate budget allows.
        if (m_dirty) {
            const LONGLONG due = m_lastRender + m_frameInterval;
            const LONGLONG qpc = QpcNow();
            if (qpc >= due) {
                Render();
                m_dirty = false;
            } else {
                LARGE_INTEGER relative;
                relative.QuadPart = -std::max<LONGLONG>(1, (due - qpc) * 10000000 / QpcFrequency());
                SetWaitableTimer(m_timer.get(), &relative, 0, nullptr, nullptr, FALSE);
            }
        }

        // 4. Sleep until a producer, the UI, the pacing timer or a deadline wakes us.
        handles.assign({ m_wake.get(), m_timer.get() });
        handleOwners.assign({ { nullptr, false }, { nullptr, false } });
        for (auto& source : m_sources) {
            if (source->frames.FrameEvent()) {
                handles.push_back(source->frames.FrameEvent());
                handleOwners.push_back({ source.get(), false });
            }
            if (source->process) {
                handles.push_back(source->process.get());
                handleOwners.push_back({ source.get(), true });
            }
        }
        const ULONGLONG after = GetTickCount64();
        const DWORD timeout = nextDeadline > after ? (DWORD)std::min<ULONGLONG>(nextDeadline - after, INFINITE - 1) : 0;
        const DWORD result = WaitForMultipleObjects((DWORD)handles.size(), handles.data(), FALSE, timeout);

        if (result == WAIT_TIMEOUT) {
            if (GetTickCount64() >= m_lastPublish + kHeartbeatMs)
                Heartbeat();
        } else if (result >= WAIT_OBJECT_0 + 2 && result < WAIT_OBJECT_0 + handles.size()) {
            auto [source, isProcess] = handleOwners[result - WAIT_OBJECT_0];
            if (isProcess) {
                // Producer exited: forget it now rather than waiting for a timeout.
                source->frames.Close();
                source->process.reset();
                source->nextAttempt = GetTickCount64() + kRetryMs;
            }
            m_dirty = true;
        }
        // m_wake / m_timer need no handling beyond looping.
    }
}

void Broker::SyncSources(ULONGLONG now, ULONGLONG& nextDeadline)
{
    std::vector<DWORD> wanted;
    if (m_activeLayout.mode == LayoutMode::Grid) {
        if (now >= m_nextGridScan) {
            m_gridPids.clear();
            for (const auto& producer : DiscoverProducers(m_adapter))
                m_gridPids.push_back(producer.pid);
            m_nextGridScan = now + kGridScanMs;
        }
        wanted = m_gridPids;
        nextDeadline = std::min(nextDeadline, m_nextGridScan);
    } else {
        for (DWORD pid : { m_activeLayout.main, m_activeLayout.pip[0], m_activeLayout.pip[1], m_activeLayout.pip[2], m_activeLayout.pip[3] })
            if (pid && std::find(wanted.begin(), wanted.end(), pid) == wanted.end())
                wanted.push_back(pid);
    }
    if (wanted.size() > kMaxSources)
        wanted.resize(kMaxSources);

    // Drop sources no longer wanted.
    const size_t before = m_sources.size();
    m_sources.erase(std::remove_if(m_sources.begin(), m_sources.end(), [&](const auto& s) {
        return std::find(wanted.begin(), wanted.end(), s->pid) == wanted.end();
    }), m_sources.end());
    if (m_sources.size() != before)
        m_dirty = true;

    // Add new ones, then (re)connect whatever is not connected.
    for (DWORD pid : wanted) {
        if (std::none_of(m_sources.begin(), m_sources.end(), [pid](const auto& s) { return s->pid == pid; })) {
            auto source = std::make_unique<Source>();
            source->pid = pid;
            m_sources.push_back(std::move(source));
        }
    }
    for (auto& source : m_sources) {
        if (source->frames.IsOpen() && source->frames.IsStale()) {
            source->frames.Close();   // producer resized: reopen right away
            source->nextAttempt = now;
        }
        if (source->frames.IsOpen())
            continue;
        if (now < source->nextAttempt) {
            nextDeadline = std::min(nextDeadline, source->nextAttempt);
            continue;
        }
        if (!source->process)
            source->process.reset(OpenProcess(SYNCHRONIZE, FALSE, source->pid));
        if (SUCCEEDED(source->frames.Open(m_device.get(), Ipc::ProducerManifestName(source->pid), true))) {
            m_dirty = true;
        } else {
            source->nextAttempt = now + kRetryMs;
            nextDeadline = std::min(nextDeadline, source->nextAttempt);
        }
    }

    UINT live = 0;
    for (const auto& source : m_sources)
        live += source->Live() ? 1 : 0;
    auto lock = m_controlLock.lock();
    if (m_status.wanted != wanted.size() || m_status.live != live) {
        m_status.wanted = (UINT)wanted.size();
        m_status.live = live;
        Notify();
    }
}

void Broker::Render()
{
    auto lock = m_renderLock.lock();
    if (!m_outputTarget)
        return;
    const UINT width = m_publisher.Width();
    const UINT height = m_publisher.Height();
    const float W = (float)width, H = (float)height;

    auto find = [this](DWORD pid) -> Source* {
        for (auto& s : m_sources)
            if (s->pid == pid && s->frames.IsOpen())
                return s.get();
        return nullptr;
    };

    // Plan the layers from the sources' sizes, then pull the frames (with mips
    // regenerated only for sources drawn smaller than their native size;
    // magnified sources sample mip 0 and never touch the chain).
    struct Planned { Source* source; CompositorLayer layer; };
    std::vector<Planned> plan;
    Source* mainSource = nullptr;

    if (m_activeLayout.mode == LayoutMode::Single) {
        if (Source* main = find(m_activeLayout.main)) {
            CompositorLayer layer;
            layer.rect = Gpu::FitRect({ 0, 0, W, H }, main->frames.Width(), main->frames.Height());
            plan.push_back({ main, layer });
            mainSource = main;
        }
        const float margin = std::round(W * 0.02f);
        for (int corner = 0; corner < 4; corner++) {
            Source* pip = find(m_activeLayout.pip[corner]);
            if (!pip)
                continue;
            const float aspect = (float)pip->frames.Width() / std::max(1u, pip->frames.Height());
            float w = W * 0.24f, h = w / aspect;
            if (h > H * 0.32f) { h = H * 0.32f; w = h * aspect; }
            const float x = (corner & 1) ? W - margin - w : margin;
            const float y = (corner & 2) ? H - margin - h : margin;
            CompositorLayer layer;
            layer.rect = { x, y, w, h };
            layer.cornerRadius = H * 0.018f;
            layer.shadowRadius = H * 0.028f;
            layer.outlineOpacity = 0.10f;
            plan.push_back({ pip, layer });
        }
    } else {
        std::vector<Source*> cells;
        for (auto& s : m_sources)
            if (s->frames.IsOpen())
                cells.push_back(s.get());
        if (!cells.empty()) {
            const UINT columns = (UINT)std::ceil(std::sqrt((double)cells.size()));
            const UINT rows = (UINT)((cells.size() + columns - 1) / columns);
            const float gap = std::round(W * 0.012f);
            const float cellW = (W - gap * (columns + 1)) / columns;
            const float cellH = (H - gap * (rows + 1)) / rows;
            for (size_t i = 0; i < cells.size(); i++) {
                const float x = gap + (i % columns) * (cellW + gap);
                const float y = gap + (i / columns) * (cellH + gap);
                CompositorLayer layer;
                layer.rect = Gpu::FitRect({ x, y, cellW, cellH }, cells[i]->frames.Width(), cells[i]->frames.Height());
                layer.cornerRadius = H * 0.012f;
                layer.shadowRadius = H * 0.015f;
                plan.push_back({ cells[i], layer });
            }
        }
    }

    for (auto& s : m_sources)
        s->drawnSmall = false;
    for (auto& p : plan)
        p.source->drawnSmall |= p.layer.rect.w < (float)p.source->frames.Width();
    for (auto& s : m_sources)
        if (s->frames.IsOpen())
            s->frames.Acquire(m_context.get(), s->drawnSmall);

    std::vector<CompositorLayer> layers;
    for (auto& p : plan) {
        if (!p.source->Live())
            continue;   // connected but no first frame yet
        p.layer.view = p.source->frames.View();
        layers.push_back(p.layer);
    }
    // "NO SIGNAL" shows behind everything until the main source (or, in grid
    // mode, any source) delivers its first frame.
    const bool covered = m_activeLayout.mode == LayoutMode::Grid ? !layers.empty() : (mainSource && mainSource->Live());

    m_compositor.Draw(m_outputTarget.get(), width, height,
                      covered ? nullptr : m_noSignalView.get(), 1920, 1080, layers);
    m_publisher.Publish(m_context.get());
    m_lastRender = QpcNow();
    m_lastPublish = GetTickCount64();
    if (m_frameNotifications)
        Notify();
}

void Broker::Heartbeat()
{
    auto lock = m_renderLock.lock();
    // Same pixels, new frame value: tells the camera the broker is alive.
    m_publisher.Publish(m_context.get());
    m_lastPublish = GetTickCount64();
}
