#include "pch.h"
#include "Tools.h" 
#include "BrokerClient.h"
#include <tlhelp32.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>

#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

const WCHAR* BROKER_MANIFEST_NAME = L"Global\\DirectPort_Producer_Manifest_VirtuaCast_Broker";

// These shaders are now for blitting the BROKER output, not for OFF mode.
// OFF mode will use the ShaderModule.
const char* g_BlitVertexShader = R"(
struct VS_OUTPUT {
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD;
};

VS_OUTPUT main(uint id : SV_VertexID) {
    VS_OUTPUT output;
    output.Tex = float2((id << 1) & 2, id & 2);
    output.Pos = float4(output.Tex.x * 2.0 - 1.0, 1.0 - output.Tex.y * 2.0, 0, 1);
    return output;
}
)";

const char* g_BlitPixelShader = R"(
Texture2D    g_texture : register(t0);
SamplerState g_sampler : register(s0);
struct VS_OUTPUT {
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD;
};
float4 main(VS_OUTPUT input) : SV_TARGET {
    return g_texture.Sample(g_sampler, input.Tex);
}
)";

HRESULT BrokerClient::ReconfigureFormat(UINT width, UINT height)
{
    RETURN_HR_IF(E_UNEXPECTED, !_dxgiManager);

    wil::com_ptr_nothrow<ID3D11Device> device;
    RETURN_IF_FAILED(_dxgiManager->GetVideoService(_deviceHandle, IID_PPV_ARGS(&device)));

    _texture.reset();
    _textureRTV.reset();
    _converter.reset();

    CD3D11_TEXTURE2D_DESC desc(DXGI_FORMAT_B8G8R8A8_UNORM, width, height, 1, 1, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
    RETURN_IF_FAILED(device->CreateTexture2D(&desc, nullptr, &_texture));
    RETURN_IF_FAILED(device->CreateRenderTargetView(_texture.get(), nullptr, &_textureRTV));
    _width = width; _height = height;

    RETURN_IF_FAILED(CoCreateInstance(CLSID_VideoProcessorMFT, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&_converter)));
    
    wil::com_ptr_nothrow<IMFMediaType> iType;
    RETURN_IF_FAILED(MFCreateMediaType(&iType));
    iType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    iType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    MFSetAttributeSize(iType.get(), MF_MT_FRAME_SIZE, width, height);
    RETURN_IF_FAILED(_converter->SetInputType(0, iType.get(), 0));

    wil::com_ptr_nothrow<IMFMediaType> oType;
    RETURN_IF_FAILED(MFCreateMediaType(&oType));
    oType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    oType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(oType.get(), MF_MT_FRAME_SIZE, width, height);
    RETURN_IF_FAILED(_converter->SetOutputType(0, oType.get(), 0));

    wil::com_ptr_nothrow<IUnknown> manager;
    _dxgiManager.query_to(&manager);
    RETURN_IF_FAILED(_converter->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)manager.get()));

    return S_OK;
}

void BrokerClient::DisconnectFromProducer()
{
    if (!_producer.isConnected) return;
    if (_producer.pManifestView) UnmapViewOfFile(_producer.pManifestView);
    if (_producer.hManifest) CloseHandle(_producer.hManifest);
    _producer = {};
    _producerPrivateTexture.reset();
    _producerSRV.reset();
    _brokerState = BrokerState::Searching;
}

HRESULT BrokerClient::CreateBlitResources()
{
    if (_blitVS && _blitPS && _blitSampler) return S_OK;
    wil::com_ptr_nothrow<ID3D11Device> device;
    RETURN_IF_FAILED(_dxgiManager->GetVideoService(_deviceHandle, IID_PPV_ARGS(&device)));
    wil::com_ptr_nothrow<ID3DBlob> vsBlob;
    wil::com_ptr_nothrow<ID3DBlob> psBlob;
    RETURN_IF_FAILED(D3DCompile(g_BlitVertexShader, strlen(g_BlitVertexShader), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, nullptr));
    RETURN_IF_FAILED(D3DCompile(g_BlitPixelShader, strlen(g_BlitPixelShader), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, nullptr));
    RETURN_IF_FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &_blitVS));
    RETURN_IF_FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &_blitPS));
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    RETURN_IF_FAILED(device->CreateSamplerState(&sampDesc, &_blitSampler));

    return S_OK;
}

HRESULT BrokerClient::FindAndConnectToBroker()
{
    if (_producer.isConnected) {
        if (WaitForSingleObject(_producer.hManifest, 0) == WAIT_ABANDONED) { DisconnectFromProducer(); }
        return S_OK;
    }
    auto now = std::chrono::steady_clock::now();
    if (_brokerState == BrokerState::Failed && (now - _lastProducerSearchTime < std::chrono::seconds(2))) {
        return S_OK;
    }
    _lastProducerSearchTime = now;

    HANDLE hManifest = OpenFileMappingW(FILE_MAP_READ, FALSE, BROKER_MANIFEST_NAME);
    if (!hManifest) { _brokerState = BrokerState::Failed; return S_OK; }
    wil::unique_handle manifest_closer(hManifest);
    BroadcastManifest* pManifestView = (BroadcastManifest*)MapViewOfFile(hManifest, FILE_MAP_READ, 0, 0, sizeof(BroadcastManifest));
    if (!pManifestView) { _brokerState = BrokerState::Failed; return S_OK; }

    wil::com_ptr_nothrow<ID3D11Device> device;
    THROW_IF_FAILED(_dxgiManager->GetVideoService(_deviceHandle, IID_PPV_ARGS(&device)));
    wil::com_ptr_nothrow<IDXGIDevice> dxgiDevice;
    wil::com_ptr_nothrow<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc;
    if (SUCCEEDED(device->QueryInterface(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc))) {
        // if (memcmp(&pManifestView->adapterLuid, &desc.AdapterLuid, sizeof(LUID)) == 0) { // <-- REMOVED LUID CHECK FOR ROBUSTNESS
            wil::com_ptr_nothrow<ID3D11Device1> device1; wil::com_ptr_nothrow<ID3D11Device5> device5;
            device->QueryInterface(&device1); device->QueryInterface(&device5);
            if (device1 && device5) {
                wil::unique_handle hTexture(GetHandleFromName(pManifestView->textureName));
                if (hTexture && SUCCEEDED(device1->OpenSharedResource1(hTexture.get(), IID_PPV_ARGS(&_producer.sharedTexture)))) {
                    wil::unique_handle hFence(GetHandleFromName(pManifestView->fenceName));
                    if (hFence && SUCCEEDED(device5->OpenSharedFence(hFence.get(), IID_PPV_ARGS(&_producer.sharedFence)))) {
                        RETURN_IF_FAILED(CreateBlitResources());
                        D3D11_TEXTURE2D_DESC pDesc;
                        _producer.sharedTexture->GetDesc(&pDesc);
                        pDesc.Usage = D3D11_USAGE_DEFAULT; pDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                        pDesc.CPUAccessFlags = 0; pDesc.MiscFlags = 0;
                        RETURN_IF_FAILED(device->CreateTexture2D(&pDesc, nullptr, &_producerPrivateTexture));
                        RETURN_IF_FAILED(device->CreateShaderResourceView(_producerPrivateTexture.get(), nullptr, &_producerSRV));
                        _producer.isConnected = true;
                        _producer.hManifest = manifest_closer.release();
                        _producer.pManifestView = pManifestView;
                        _brokerState = BrokerState::Connected;
                        return S_OK;
                    }
                }
            }
        // }
    }
    UnmapViewOfFile(pManifestView);
    _brokerState = BrokerState::Failed;
    return S_OK;
}

HRESULT BrokerClient::SetD3DManager(IUnknown* manager, UINT width, UINT height)
{
    RETURN_HR_IF_NULL(E_POINTER, manager);
    RETURN_HR_IF(E_INVALIDARG, !width || !height);
    _dxgiManager.reset();
    RETURN_IF_FAILED(manager->QueryInterface(&_dxgiManager));
    if(_deviceHandle) { _dxgiManager->CloseDeviceHandle(_deviceHandle); _deviceHandle = nullptr; }
    RETURN_IF_FAILED(_dxgiManager->OpenDeviceHandle(&_deviceHandle));
    
    // Initialize the off-mode shader here using the device obtained from the D3DManager
    wil::com_ptr_nothrow<ID3D11Device> device;
    RETURN_IF_FAILED(_dxgiManager->GetVideoService(_deviceHandle, IID_PPV_ARGS(&device)));
    RETURN_IF_FAILED(_offModeShader.Initialize(device)); // Initialize the ShaderModule

    return ReconfigureFormat(width, height);
}

HRESULT BrokerClient::Generate(IMFSample* sample, REFGUID format, IMFSample** outSample)
{
    RETURN_HR_IF_NULL(E_POINTER, sample); RETURN_HR_IF_NULL(E_POINTER, outSample);
    *outSample = nullptr;
    RETURN_HR_IF(E_UNEXPECTED, !HasD3DManager());
    wil::com_ptr_nothrow<ID3D11Device> device;
    RETURN_IF_FAILED(_dxgiManager->GetVideoService(_deviceHandle, IID_PPV_ARGS(&device)));
    wil::com_ptr_nothrow<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    FindAndConnectToBroker();

    if (_producer.isConnected) {
        UINT64 latestFrame = _producer.pManifestView->frameValue;
        if (latestFrame > _producer.lastSeenFrame) {
            wil::com_ptr_nothrow<ID3D11DeviceContext4> context4; context->QueryInterface(&context4);
            if (context4) {
                context4->Wait(_producer.sharedFence.get(), latestFrame);
                context->CopyResource(_producerPrivateTexture.get(), _producer.sharedTexture.get());
                _producer.lastSeenFrame = latestFrame;
            }
        }
        
        D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)_width, (float)_height, 0.0f, 1.0f };
        context->RSSetViewports(1, &vp);
        context->OMSetRenderTargets(1, _textureRTV.addressof(), nullptr);

        context->VSSetShader(_blitVS.get(), nullptr, 0);
        context->PSSetShader(_blitPS.get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, _producerSRV.addressof());
        context->PSSetSamplers(0, 1, _blitSampler.addressof());
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->Draw(3, 0);

    } else {
        // Use the new ShaderModule for "OFF" mode
        _offModeShader.Render(_textureRTV, _width, _height);
    }
    context->Flush();

    wil::com_ptr_nothrow<IMFMediaBuffer> mediaBuffer;
    RETURN_IF_FAILED(sample->RemoveAllBuffers());
    RETURN_IF_FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), _texture.get(), 0, 0, &mediaBuffer));
    RETURN_IF_FAILED(sample->AddBuffer(mediaBuffer.get()));

    if (format == MFVideoFormat_NV12) {
        RETURN_HR_IF_NULL(E_UNEXPECTED, _converter);
        RETURN_IF_FAILED(_converter->ProcessInput(0, sample, 0));
        MFT_OUTPUT_DATA_BUFFER buffer = {}; buffer.pSample = nullptr;
        DWORD status = 0;
        HRESULT hr = _converter->ProcessOutput(0, 1, &buffer, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK; 
        RETURN_IF_FAILED(hr);
        *outSample = buffer.pSample;
    } else {
        sample->AddRef();
        *outSample = sample;
    }
    return S_OK;
}