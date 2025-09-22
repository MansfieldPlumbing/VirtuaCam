#pragma once
#include <chrono>
#include <d3d11_4.h>
#include <DirectXMath.h>
#include "App.h"
#include "Utilities.h"

class BrokerClient
{
    UINT _width;
    UINT _height;
    HANDLE _deviceHandle;
    wil::com_ptr_nothrow<IMFDXGIDeviceManager> _dxgiManager;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> _texture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _textureRTV;
    
    Microsoft::WRL::ComPtr<IMFTransform> _converter;
    
    ProducerConnection _producer;
    std::chrono::steady_clock::time_point _lastProducerSearchTime;
    BrokerState _brokerState;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> _producerPrivateTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _producerSRV;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> _blitVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> _blitPS;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> _blitSampler;

    HRESULT FindAndConnectToBroker();
    void DisconnectFromProducer();
    HRESULT CreateBlitResources();
    
public:
    BrokerClient() :
        _width(0),
        _height(0),
        _deviceHandle(nullptr),
        _producer{},
        _lastProducerSearchTime{},
        _brokerState(BrokerState::Searching)
    {
    }

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