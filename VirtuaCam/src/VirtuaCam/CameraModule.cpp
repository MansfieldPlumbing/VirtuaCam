#define WIN32_LEAN_AND_MEAN
#include "pch.h"
#include <wrl.h>
#include "CameraModule.h"
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <sddl.h>
#include <string>
#include <atomic>
#include <vector>
#include <map>
#include <memory>
#include <wil/com.h>
#include <wil/result.h>
#include <wil/resource.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "advapi32.lib")

using namespace Microsoft::WRL;

namespace VirtuaCam {

    class CameraSource
    {
    public:
        ~CameraSource();
        HRESULT Initialize(int cameraId, const std::wstring& manifestPrefix);
        void Shutdown();
        void ProcessFrame();
        bool IsCapturing() { return m_isCapturing; }

    private:
        HRESULT InitD3D11();
        void CleanupD3D11();
        void CleanupCaptureResources();

        ComPtr<ID3D11Device> m_d3d11Device;
        ComPtr<ID3D11Device5> m_d3d11Device5;
        ComPtr<ID3D11DeviceContext> m_d3d11Context;
        ComPtr<ID3D11DeviceContext4> m_d3d11Context4;
        
        ComPtr<ID3D11Texture2D> m_privateStagingTexture;
        ComPtr<ID3D11Texture2D> m_sharedD3D11Texture;
        ComPtr<ID3D11Fence> m_sharedD3D11Fence;
        HANDLE m_hSharedTextureHandle = nullptr;
        HANDLE m_hSharedFenceHandle = nullptr;
        HANDLE m_hManifest = nullptr;
        BroadcastManifest* m_pManifestView = nullptr;
        std::atomic<UINT64> m_fenceValue = 0;

        ComPtr<IMFSourceReader> m_sourceReader;
        int m_activeCameraId = -1;
        long m_videoWidth = 0, m_videoHeight = 0;
        std::atomic<bool> m_isCapturing = false;
    };

    static std::unique_ptr<CameraSource> g_cameraSource;

    CameraSource::~CameraSource() {
        Shutdown();
    }

    void CameraSource::Shutdown() {
        if (!m_isCapturing.exchange(false)) return;

        if (m_sourceReader) {
            m_sourceReader->Flush(MF_SOURCE_READER_ALL_STREAMS);
        }
        
        CleanupCaptureResources();
        CleanupD3D11();
    }

    void CameraSource::CleanupCaptureResources() {
        m_sourceReader.Reset();
        if (m_pManifestView) UnmapViewOfFile(m_pManifestView);
        if (m_hManifest) CloseHandle(m_hManifest);
        m_pManifestView = nullptr; m_hManifest = nullptr;
        if (m_hSharedTextureHandle) CloseHandle(m_hSharedTextureHandle);
        if (m_hSharedFenceHandle) CloseHandle(m_hSharedFenceHandle);
        m_hSharedTextureHandle = nullptr; m_hSharedFenceHandle = nullptr;
        m_sharedD3D11Fence.Reset();
        m_sharedD3D11Texture.Reset();
        m_privateStagingTexture.Reset();
        m_activeCameraId = -1;
    }

    void CameraSource::CleanupD3D11()
    {
        if(m_d3d11Context) m_d3d11Context->ClearState();
        m_d3d11Context.Reset();
        m_d3d11Context4.Reset();
        m_d3d11Device.Reset();
        m_d3d11Device5.Reset();
    }

    HRESULT CameraSource::Initialize(int cameraId, const std::wstring& manifestPrefix)
    {
        RETURN_IF_FAILED(InitD3D11());

        ComPtr<IMFAttributes> pAttributes;
        RETURN_IF_FAILED(MFCreateAttributes(&pAttributes, 1));
        RETURN_IF_FAILED(pAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));

        UINT32 count = 0;
        IMFActivate** devices = nullptr;
        RETURN_IF_FAILED(MFEnumDeviceSources(pAttributes.Get(), &devices, &count));
        if (count == 0 || (UINT)cameraId >= count) {
            if (devices) CoTaskMemFree(devices);
            return E_FAIL;
        }

        ComPtr<IMFMediaSource> pSource;
        RETURN_IF_FAILED(devices[cameraId]->ActivateObject(IID_PPV_ARGS(&pSource)));
        for (UINT i = 0; i < count; ++i) devices[i]->Release();
        CoTaskMemFree(devices);

        ComPtr<IMFAttributes> pReaderAttributes;
        RETURN_IF_FAILED(MFCreateAttributes(&pReaderAttributes, 2));
        RETURN_IF_FAILED(pReaderAttributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE));
        RETURN_IF_FAILED(pReaderAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE));

        RETURN_IF_FAILED(MFCreateSourceReaderFromMediaSource(pSource.Get(), pReaderAttributes.Get(), &m_sourceReader));

        ComPtr<IMFMediaType> outputType;
        RETURN_IF_FAILED(MFCreateMediaType(&outputType));
        RETURN_IF_FAILED(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
        RETURN_IF_FAILED(outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32));

        bool found_compatible_format = false;
        for (DWORD i = 0; ; ++i) {
            ComPtr<IMFMediaType> nativeType;
            HRESULT hr = m_sourceReader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &nativeType);
            if (hr == MF_E_NO_MORE_TYPES) break;
            RETURN_IF_FAILED(hr);
            
            hr = m_sourceReader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, nativeType.Get());
            if (SUCCEEDED(hr)) {
                hr = m_sourceReader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, outputType.Get());
                if (SUCCEEDED(hr)) {
                    found_compatible_format = true;
                    break;
                }
            }
        }
        if (!found_compatible_format) return E_FAIL;

        ComPtr<IMFMediaType> pCurrentType;
        RETURN_IF_FAILED(m_sourceReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCurrentType));
        MFGetAttributeSize(pCurrentType.Get(), MF_MT_FRAME_SIZE, (UINT32*)&m_videoWidth, (UINT32*)&m_videoHeight);

        D3D11_TEXTURE2D_DESC td{};
        td.Width = m_videoWidth; td.Height = m_videoHeight; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = 0;
        RETURN_IF_FAILED(m_d3d11Device->CreateTexture2D(&td, nullptr, &m_privateStagingTexture));

        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        RETURN_IF_FAILED(m_d3d11Device->CreateTexture2D(&td, nullptr, &m_sharedD3D11Texture));

        RETURN_IF_FAILED(m_d3d11Device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_sharedD3D11Fence)));

        std::wstring manifestName = manifestPrefix + std::to_wstring(GetCurrentProcessId());
        wil::unique_hlocal_security_descriptor sd;
        PSECURITY_DESCRIPTOR sd_ptr = nullptr;
        THROW_IF_WIN32_BOOL_FALSE(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd_ptr, NULL));
        sd.reset(sd_ptr);
        SECURITY_ATTRIBUTES sa = { sizeof(sa), sd.get(), FALSE };
        m_hManifest = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(BroadcastManifest), manifestName.c_str());
        if (!m_hManifest) return HRESULT_FROM_WIN32(GetLastError());

        m_pManifestView = (BroadcastManifest*)MapViewOfFile(m_hManifest, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BroadcastManifest));
        if (!m_pManifestView) return HRESULT_FROM_WIN32(GetLastError());

        ZeroMemory(m_pManifestView, sizeof(BroadcastManifest));
        m_pManifestView->width = m_videoWidth; m_pManifestView->height = m_videoHeight;
        m_pManifestView->format = DXGI_FORMAT_B8G8R8A8_UNORM;

        ComPtr<IDXGIDevice> dxgiDevice; m_d3d11Device.As(&dxgiDevice);
        ComPtr<IDXGIAdapter> adapter; dxgiDevice->GetAdapter(&adapter);
        DXGI_ADAPTER_DESC desc; adapter->GetDesc(&desc);
        m_pManifestView->adapterLuid = desc.AdapterLuid;
        
        std::wstring texName = L"Global\\VirtuaCam_CameraTexture_" + std::to_wstring(GetCurrentProcessId());
        std::wstring fenceName = L"Global\\VirtuaCam_CameraFence_" + std::to_wstring(GetCurrentProcessId());
        wcscpy_s(m_pManifestView->textureName, _countof(m_pManifestView->textureName), texName.c_str());
        wcscpy_s(m_pManifestView->fenceName, _countof(m_pManifestView->fenceName), fenceName.c_str());

        ComPtr<IDXGIResource1> r1; m_sharedD3D11Texture.As(&r1);
        RETURN_IF_FAILED(r1->CreateSharedHandle(&sa, GENERIC_ALL, texName.c_str(), &m_hSharedTextureHandle));
        RETURN_IF_FAILED(m_sharedD3D11Fence->CreateSharedHandle(&sa, GENERIC_ALL, fenceName.c_str(), &m_hSharedFenceHandle));

        m_activeCameraId = cameraId;
        m_isCapturing = true;
        return S_OK;
    }
    
    HRESULT CameraSource::InitD3D11() {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        RETURN_IF_FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &m_d3d11Device, nullptr, &m_d3d11Context));
        RETURN_IF_FAILED(m_d3d11Device.As(&m_d3d11Device5));
        RETURN_IF_FAILED(m_d3d11Context.As(&m_d3d11Context4));
        return S_OK;
    }

    void CameraSource::ProcessFrame()
    {
        if (!m_isCapturing || !m_sourceReader) return;

        ComPtr<IMFSample> pSample;
        DWORD streamFlags;
        LONGLONG timestamp;
        HRESULT hr = m_sourceReader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, &streamFlags, &timestamp, &pSample);
        if (FAILED(hr) || !pSample) return;

        ComPtr<IMFMediaBuffer> pBuffer;
        THROW_IF_FAILED(pSample->ConvertToContiguousBuffer(&pBuffer));

        BYTE* pData = nullptr;
        DWORD cbCurrentLength = 0;
        THROW_IF_FAILED(pBuffer->Lock(&pData, NULL, &cbCurrentLength));

        m_d3d11Context->UpdateSubresource(m_privateStagingTexture.Get(), 0, NULL, pData, m_videoWidth * 4, 0);
        
        THROW_IF_FAILED(pBuffer->Unlock());
        
        m_d3d11Context->CopyResource(m_sharedD3D11Texture.Get(), m_privateStagingTexture.Get());

        UINT64 newFenceValue = m_fenceValue.fetch_add(1) + 1;
        m_d3d11Context4->Signal(m_sharedD3D11Fence.Get(), newFenceValue);
        if (m_pManifestView) {
            InterlockedExchange64(reinterpret_cast<volatile LONGLONG*>(&m_pManifestView->frameValue), newFenceValue);
        }
    }
}

extern "C" {
    CAMERAMODULE_API HRESULT InitializeProducer(int cameraIndex, const wchar_t* manifestPrefix)
    {
        VirtuaCam::g_cameraSource = std::make_unique<VirtuaCam::CameraSource>();
        return VirtuaCam::g_cameraSource->Initialize(cameraIndex, manifestPrefix);
    }

    CAMERAMODULE_API void RunProducer()
    {
        if (VirtuaCam::g_cameraSource && VirtuaCam::g_cameraSource->IsCapturing())
        {
            while (VirtuaCam::g_cameraSource->IsCapturing())
            {
                VirtuaCam::g_cameraSource->ProcessFrame();
                Sleep(1);
            }
        }
    }

    CAMERAMODULE_API void ShutdownProducer()
    {
        if (VirtuaCam::g_cameraSource)
        {
            VirtuaCam::g_cameraSource->Shutdown();
            VirtuaCam::g_cameraSource.reset();
        }
    }
}