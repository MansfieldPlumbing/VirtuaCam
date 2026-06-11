// =============================================================================
// Multiplexer.cpp  --  GPU frame compositing
// =============================================================================
// The Multiplexer composites frames from multiple producer processes into a
// single 1920x1080 output texture.  It supports two layout modes:
//
//   Priority-list mode (default)
//     producers[0]  — primary source, rendered fullscreen as the background.
//     producers[1..4] — up to 4 picture-in-picture overlays, rendered as
//                        ¼-width x ¼-height tiles at the four corners.
//
//   Grid mode
//     All discovered producers are tiled evenly.
//     (Currently stubbed — the grid tile layout is not yet implemented.)
//
// When no primary source is available, a static black "NO SIGNAL" frame is
// shown instead of leaving the output blank.
//
// All rendering uses D3D11 and the same full-screen triangle blit technique
// as BrokerClient: a vertex shader generates three vertices from SV_VertexID,
// forming a triangle that covers the entire viewport without a vertex buffer.
// =============================================================================

#include "pch.h"
#include "Multiplexer.h"
#include <d3dcompiler.h>
#include <cmath>
#include <algorithm>

#pragma comment(lib, "d3dcompiler.lib")

// ---------------------------------------------------------------------------
// Blit shaders  (same technique as BrokerClient — see that file for details)
// ---------------------------------------------------------------------------

const char* g_BlitVertexShader = R"(
struct VS_OUTPUT { float4 Pos : SV_POSITION; float2 Tex : TEXCOORD; };
VS_OUTPUT main(uint id : SV_VertexID) {
    VS_OUTPUT output;
    output.Tex = float2((id << 1) & 2, id & 2);
    output.Pos = float4(output.Tex.x * 2.0 - 1.0, 1.0 - output.Tex.y * 2.0, 0, 1);
    return output;
})";

const char* g_BlitPixelShader = R"(
Texture2D    g_texture : register(t0);
SamplerState g_sampler : register(s0);
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET {
    return g_texture.Sample(g_sampler, uv);
})";

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Multiplexer::Multiplexer() {}
Multiplexer::~Multiplexer() {}

HRESULT Multiplexer::Initialize(Microsoft::WRL::ComPtr<ID3D11Device> device)
{
    m_device = device;
    m_device->GetImmediateContext(&m_context);
    m_context.As(&m_context4);
    RETURN_IF_FAILED(CreateResources());
    return S_OK;
}

void Multiplexer::Shutdown()
{
    m_noSignalTexture.Reset();
    m_device.Reset();
    m_context.Reset();
    m_context4.Reset();
    m_compositeTexture.Reset();
    m_compositeRTV.Reset();
    m_outputTexture.Reset();
    m_outputFence.Reset();
    m_blitVS.Reset();
    m_blitPS.Reset();
    m_blitSampler.Reset();
    m_producerResources.clear();
}

// ---------------------------------------------------------------------------
// Output accessors  (used by Broker.cpp to copy the composited frame)
// ---------------------------------------------------------------------------

ID3D11Texture2D* Multiplexer::GetOutputTexture()   { return m_outputTexture.Get(); }
ID3D11Fence*     Multiplexer::GetOutputFence()      { return m_outputFence.Get(); }
UINT64           Multiplexer::GetOutputFrameValue() { return m_outputFrameValue; }

// ---------------------------------------------------------------------------
// CreateResources
// ---------------------------------------------------------------------------
// Two textures are used:
//   m_compositeTexture  — the render target; producers are drawn into this.
//   m_outputTexture     — a non-shared copy; Broker.cpp CopyResources this into
//                         the shared NT-handle texture so consumers can read it.
// Keeping them separate avoids a DXGI_RESOURCE_MISC_SHARED binding constraint
// that would prevent using m_compositeTexture as a render target.

HRESULT Multiplexer::CreateResources()
{
    D3D11_TEXTURE2D_DESC compositeDesc = {};
    compositeDesc.Width          = 1920;
    compositeDesc.Height         = 1080;
    compositeDesc.MipLevels      = 1;
    compositeDesc.ArraySize      = 1;
    compositeDesc.Format         = DXGI_FORMAT_B8G8R8A8_UNORM;
    compositeDesc.SampleDesc.Count = 1;
    compositeDesc.Usage          = D3D11_USAGE_DEFAULT;
    compositeDesc.BindFlags      = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    RETURN_IF_FAILED(m_device->CreateTexture2D(&compositeDesc, nullptr, &m_compositeTexture));
    RETURN_IF_FAILED(m_device->CreateRenderTargetView(m_compositeTexture.Get(), nullptr, &m_compositeRTV));

    // Output texture: same dimensions/format but no MISC flags (can be freely
    // used as a CopyResource destination).
    D3D11_TEXTURE2D_DESC outputDesc = compositeDesc;
    outputDesc.MiscFlags = 0;
    RETURN_IF_FAILED(m_device->CreateTexture2D(&outputDesc, nullptr, &m_outputTexture));

    // Fence for signalling Broker.cpp that a new frame has been composited.
    Microsoft::WRL::ComPtr<ID3D11Device5> device5;
    m_device.As(&device5);
    RETURN_IF_FAILED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_outputFence)));

    // Compile blit shaders once at startup.
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob;
    RETURN_IF_FAILED(D3DCompile(g_BlitVertexShader, strlen(g_BlitVertexShader), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, nullptr));
    RETURN_IF_FAILED(D3DCompile(g_BlitPixelShader,  strlen(g_BlitPixelShader),  nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, nullptr));
    RETURN_IF_FAILED(m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_blitVS));
    RETURN_IF_FAILED(m_device->CreatePixelShader( psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_blitPS));

    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD         = 0;
    sampDesc.MaxLOD         = D3D11_FLOAT32_MAX;
    RETURN_IF_FAILED(m_device->CreateSamplerState(&sampDesc, &m_blitSampler));

    // Static placeholder used as the background when no primary source is live.
    RETURN_IF_FAILED(CreateNoSignalTexture(m_device.Get(), compositeDesc.Width, compositeDesc.Height, &m_noSignalTexture));

    return S_OK;
}

// ---------------------------------------------------------------------------
// PruneConnections
// ---------------------------------------------------------------------------
// Remove GPU resources for producers that are no longer in the active list
// (i.e. the process has exited or been deselected).

void Multiplexer::PruneConnections(const std::vector<VirtuaCam::DiscoveredSharedStream>& currentProducers)
{
    m_producerResources.erase(
        std::remove_if(m_producerResources.begin(), m_producerResources.end(),
            [&](const ProducerGpuResources& res) {
                auto it = std::find_if(currentProducers.begin(), currentProducers.end(),
                    [&](const auto& p){ return p.processId == res.pid; });
                return it == currentProducers.end();
            }),
        m_producerResources.end());
}

// ---------------------------------------------------------------------------
// UpdateProducerConnection
// ---------------------------------------------------------------------------
// Open the shared texture and fence for a newly-discovered producer and create
// a private (SRV-bindable) copy of the texture.  Idempotent — does nothing if
// the producer is already connected.

HRESULT Multiplexer::UpdateProducerConnection(const VirtuaCam::DiscoveredSharedStream& streamInfo)
{
    // Already connected — nothing to do.
    for (auto& res : m_producerResources)
        if (res.pid == streamInfo.processId) return S_OK;

    ProducerGpuResources newRes;
    newRes.pid = streamInfo.processId;

    Microsoft::WRL::ComPtr<ID3D11Device1> device1;
    Microsoft::WRL::ComPtr<ID3D11Device5> device5;
    m_device.As(&device1);
    m_device.As(&device5);

    // GetHandleFromName uses a temporary D3D12 device (OpenSharedHandleByName
    // is not available on D3D11).
    wil::unique_handle hTexture(GetHandleFromName(streamInfo.textureName.c_str()));
    wil::unique_handle hFence(  GetHandleFromName(streamInfo.fenceName.c_str()));

    if (!hTexture || FAILED(device1->OpenSharedResource1(hTexture.get(), IID_PPV_ARGS(&newRes.sharedTexture)))) return E_FAIL;
    if (!hFence   || FAILED(device5->OpenSharedFence(    hFence.get(),   IID_PPV_ARGS(&newRes.sharedFence))))   return E_FAIL;

    // Create a private copy of the texture (shared textures can't be bound as SRVs).
    D3D11_TEXTURE2D_DESC sharedDesc;
    newRes.sharedTexture->GetDesc(&sharedDesc);
    sharedDesc.MiscFlags  = 0;
    sharedDesc.BindFlags  = D3D11_BIND_SHADER_RESOURCE;
    sharedDesc.Usage      = D3D11_USAGE_DEFAULT;

    RETURN_IF_FAILED(m_device->CreateTexture2D(&sharedDesc, nullptr, &newRes.privateTexture));
    RETURN_IF_FAILED(m_device->CreateShaderResourceView(newRes.privateTexture.Get(), nullptr, &newRes.privateSRV));

    newRes.connected = true;
    m_producerResources.push_back(std::move(newRes));
    return S_OK;
}

// ---------------------------------------------------------------------------
// CompositeFrames
// ---------------------------------------------------------------------------
// Main per-frame compositing entry point, called by Broker::RenderBrokerFrame().
//
// Steps:
//   1. Sync GPU resources: open connections for new producers, drop stale ones,
//      then for each producer wait on its fence and copy its texture locally.
//   2. Clear the composite render target to black.
//   3. Render background: either the primary source fullscreen, or the static
//      "NO SIGNAL" frame if no primary source is available.
//   4. Render overlays: PiP tiles (priority-list mode) or grid (grid mode).
//   5. Finalise: copy composite to output texture and signal the output fence.

void Multiplexer::CompositeFrames(const std::vector<VirtuaCam::DiscoveredSharedStream>& producers, bool isGridMode)
{
    // --- Step 1: Update connections and sync GPU resources ---

    // Build the list of actually-active producers (PID != 0).
    std::vector<VirtuaCam::DiscoveredSharedStream> activeProducers;
    std::copy_if(producers.begin(), producers.end(), std::back_inserter(activeProducers),
        [](const auto& p) { return p.processId != 0; });

    PruneConnections(activeProducers);
    for (const auto& p : activeProducers)
        UpdateProducerConnection(p);

    // For each connected producer, check if a new frame is available.
    // Wait on the producer's fence (GPU-side) then copy to our private texture.
    for (auto& res : m_producerResources) {
        wil::unique_handle hManifest(OpenFileMappingW(FILE_MAP_READ, FALSE,
            (L"DirectPort_Producer_Manifest_" + std::to_wstring(res.pid)).c_str()));
        if (!hManifest) continue;

        BroadcastManifest* pView = (BroadcastManifest*)MapViewOfFile(hManifest.get(), FILE_MAP_READ, 0, 0, sizeof(BroadcastManifest));
        if (!pView) continue;

        UINT64 latestFrame = pView->frameValue;
        if (latestFrame > res.lastSeenFrame) {
            m_context4->Wait(res.sharedFence.Get(), latestFrame);
            m_context->CopyResource(res.privateTexture.Get(), res.sharedTexture.Get());
            res.lastSeenFrame = latestFrame;
        }
        UnmapViewOfFile(pView);
    }

    // --- Step 2: Clear the composite render target ---

    m_context->OMSetRenderTargets(1, m_compositeRTV.GetAddressOf(), nullptr);
    const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    m_context->ClearRenderTargetView(m_compositeRTV.Get(), clearColor);

    D3D11_TEXTURE2D_DESC compDesc;
    m_compositeTexture->GetDesc(&compDesc);
    UINT MUX_WIDTH  = compDesc.Width;
    UINT MUX_HEIGHT = compDesc.Height;

    // Helper lambda to look up a producer's GPU resources by PID.
    auto find_resource = [&](DWORD pid) -> ProducerGpuResources* {
        for (auto& res : m_producerResources)
            if (res.pid == pid) return &res;
        return nullptr;
    };

    // --- Step 3: Render background ---

    // In priority-list mode, producers[0] is the primary (fullscreen) source.
    ProducerGpuResources* primarySourceRes = nullptr;
    if (!isGridMode && !producers.empty() && producers[0].processId != 0)
        primarySourceRes = find_resource(producers[0].processId);

    if (primarySourceRes && primarySourceRes->privateSRV) {
        // Blit the primary source at full output resolution.
        D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)MUX_WIDTH, (float)MUX_HEIGHT, 0.0f, 1.0f };
        m_context->RSSetViewports(1, &vp);
        m_context->VSSetShader(m_blitVS.Get(), nullptr, 0);
        m_context->PSSetShader(m_blitPS.Get(), nullptr, 0);
        m_context->PSSetSamplers(0, 1, m_blitSampler.GetAddressOf());
        m_context->PSSetShaderResources(0, 1, primarySourceRes->privateSRV.GetAddressOf());
        m_context->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->Draw(3, 0);
    } else if (m_noSignalTexture) {
        // No primary source — show the static "NO SIGNAL" frame.
        m_context->CopyResource(m_compositeTexture.Get(), m_noSignalTexture.Get());
        m_context->OMSetRenderTargets(1, m_compositeRTV.GetAddressOf(), nullptr);
    }

    // --- Step 4: Render overlays ---

    if (isGridMode) {
        // TODO: tile all active producers evenly across the output.
        // Grid layout is not yet implemented.
    } else {
        // PiP mode: render producers[1..4] as small overlays at the four corners.
        // Each PiP tile is ¼ of the output width/height, inset by 10px from the edge.
        m_context->VSSetShader(m_blitVS.Get(), nullptr, 0);
        m_context->PSSetShader(m_blitPS.Get(), nullptr, 0);
        m_context->PSSetSamplers(0, 1, m_blitSampler.GetAddressOf());
        m_context->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        const float pip_w  = MUX_WIDTH  / 4.0f;
        const float pip_h  = MUX_HEIGHT / 4.0f;
        const float margin = 10.0f;

        // Viewport for each PiP position: TL, TR, BL, BR
        D3D11_VIEWPORT pip_vps[4] = {
            { margin,                          margin,                          pip_w, pip_h, 0.0f, 1.0f },  // Top-left
            { MUX_WIDTH  - pip_w - margin,     margin,                          pip_w, pip_h, 0.0f, 1.0f },  // Top-right
            { margin,                          MUX_HEIGHT - pip_h - margin,    pip_w, pip_h, 0.0f, 1.0f },  // Bottom-left
            { MUX_WIDTH  - pip_w - margin,     MUX_HEIGHT - pip_h - margin,    pip_w, pip_h, 0.0f, 1.0f },  // Bottom-right
        };

        // producers[1] -> pip_vps[0] (TL), producers[2] -> pip_vps[1] (TR), etc.
        for (size_t i = 1; i < producers.size() && i < 5; ++i) {
            if (producers[i].processId != 0) {
                ProducerGpuResources* res = find_resource(producers[i].processId);
                if (res && res->privateSRV) {
                    m_context->RSSetViewports(1, &pip_vps[i - 1]);
                    m_context->PSSetShaderResources(0, 1, res->privateSRV.GetAddressOf());
                    m_context->Draw(3, 0);
                }
            }
        }
    }

    // --- Step 5: Finalise frame ---

    // Copy composite -> output texture, then increment and signal the fence so
    // Broker.cpp knows the output is ready to copy to the shared NT handle texture.
    m_context->CopyResource(m_outputTexture.Get(), m_compositeTexture.Get());
    m_outputFrameValue++;
    m_context4->Signal(m_outputFence.Get(), m_outputFrameValue);
}
