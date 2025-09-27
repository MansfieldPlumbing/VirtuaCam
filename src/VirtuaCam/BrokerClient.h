#pragma once
#include <chrono>
#include <d3d11_4.h>
#include <DirectXMath.h>
#include "Tools.h"
#include "App.h"
#include "ShaderModule.h" // <-- RE-INTRODUCED

struct ProducerConnection
{
    bool isConnected = false;
    DWORD producerPid = 0;
    HANDLE hManifest = nullptr;
    BroadcastManifest* pManifestView = nullptr;
    wil::com_ptr_nothrow<ID3D11Texture2D> sharedTexture;
    wil::com_ptr_nothrow<ID3D11Fence> sharedFence;
    UINT64 lastSeenFrame = 0;
};

class BrokerClient
{
    UINT _width;
    UINT _height;
    HANDLE _deviceHandle;
    wil::com_ptr_nothrow<IMFDXGIDeviceManager> _dxgiManager;
    wil::com_ptr_nothrow<ID3D11Texture2D> _texture;
    wil::com_ptr_nothrow<ID3D11RenderTargetView> _textureRTV;
    wil::com_ptr_nothrow<IMFTransform> _converter;
    ProducerConnection _producer;
    std::chrono::steady_clock::time_point _lastProducerSearchTime;
    BrokerState _brokerState;
    wil::com_ptr_nothrow<ID3D11Texture2D> _producerPrivateTexture;
    wil::com_ptr_nothrow<ID3D11ShaderResourceView> _producerSRV;
    wil::com_ptr_nothrow<ID3D11VertexShader> _blitVS;
    wil::com_ptr_nothrow<ID3D11PixelShader> _blitPS;
    wil::com_ptr_nothrow<ID3D11SamplerState> _blitSampler;
    ShaderModule _offModeShader; // <-- RE-INTRODUCED

private:
    HRESULT FindAndConnectToBroker();
    void DisconnectFromProducer();
    HRESULT CreateBlitResources();
    
public:
    BrokerClient() :
        _width(0), _height(0), _deviceHandle(nullptr), _producer{},
        _lastProducerSearchTime{}, _brokerState(BrokerState::Searching) {}

    ~BrokerClient()
    {
        DisconnectFromProducer();
        if (_dxgiManager && _deviceHandle)
        {
            _dxgiManager->CloseDeviceHandle(_deviceHandle);
        }
    }

    HRESULT ReconfigureFormat(UINT width, UINT height);
    HRESULT SetD3DManager(IUnknown* manager, UINT width, UINT height);
    const bool HasD3DManager() const { return _dxgiManager != nullptr; }
    HRESULT Generate(IMFSample* sample, REFGUID format, IMFSample** outSample);
};