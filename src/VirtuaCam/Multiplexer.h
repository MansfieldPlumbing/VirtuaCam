#pragma once
#include "Tools.h"
#include "Discovery.h"
#include <wrl/client.h>
#include <d3d11_4.h>
#include <vector>

class Multiplexer
{
public:
    Multiplexer();
    ~Multiplexer();

    HRESULT Initialize(Microsoft::WRL::ComPtr<ID3D11Device> device);
    void Shutdown();
    void CompositeFrames(const std::vector<VirtuaCam::DiscoveredSharedStream>& producers, bool isGridMode);
    ID3D11Texture2D* GetOutputTexture();
    ID3D11Fence* GetOutputFence();
    UINT64 GetOutputFrameValue();

private:
    HRESULT CreateResources();
    HRESULT UpdateProducerConnection(const VirtuaCam::DiscoveredSharedStream& streamInfo);
    void PruneConnections(const std::vector<VirtuaCam::DiscoveredSharedStream>& currentProducers);

    struct ProducerGpuResources {
        DWORD pid = 0;
        bool connected = false;
        UINT width = 0;      // Source dimensions, for aspect-fit layout
        UINT height = 0;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> sharedTexture;
        Microsoft::WRL::ComPtr<ID3D11Fence> sharedFence;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> privateTexture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> privateSRV;
        UINT64 lastSeenFrame = 0;
    };
    
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> m_context4;
    
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_compositeTexture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_compositeRTV;
    
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_blitVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_blitPS;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_blitSampler;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_noSignalTexture;  // Static "NO SIGNAL" frame shown when no primary source is live

    std::vector<ProducerGpuResources> m_producerResources;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_outputTexture;
    Microsoft::WRL::ComPtr<ID3D11Fence> m_outputFence;
    UINT64 m_outputFrameValue = 0;

    // Composite-skip state: the frame is only re-rendered when a producer
    // delivered a new frame or the layout (source list / mode) changed.
    std::vector<DWORD> m_lastLayoutPids;
    bool m_lastGridMode = false;
    bool m_hasComposited = false;
};