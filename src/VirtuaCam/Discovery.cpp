#include "pch.h"
#include "Discovery.h"
#include "Tools.h"
#include <d3d11_1.h>
#include <d3d12.h>
#include <tlhelp32.h>
#include <memory>

#pragma comment(lib, "d3d12.lib")

namespace VirtuaCam {

struct Discovery::Impl {
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    LUID m_adapterLuid = {};
    std::vector<DiscoveredSharedStream> m_discoveredStreams;
};

Discovery::Discovery() : pImpl(std::make_unique<Impl>()) {}
Discovery::~Discovery() { Teardown(); }

HRESULT Discovery::Initialize(ID3D11Device* device) {
    pImpl->m_device = device;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    pImpl->m_device.As(&dxgiDevice);
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC desc;
    adapter->GetDesc(&desc);
    pImpl->m_adapterLuid = desc.AdapterLuid;
    return S_OK;
}

void Discovery::Teardown() {
    pImpl->m_device.Reset();
}

void Discovery::DiscoverStreams() {
    pImpl->m_discoveredStreams.clear();
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe32 = {};
    pe32.dwSize = sizeof(PROCESSENTRY32W);
    
    const std::vector<std::pair<std::wstring, std::wstring>> producerSignatures = {
        { L"DirectPort_Producer_Manifest_", L"DirectPort" },
    };

    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            for (const auto& sig : producerSignatures) {
                std::wstring manifestName = sig.first + std::to_wstring(pe32.th32ProcessID);
                HANDLE hManifest = OpenFileMappingW(FILE_MAP_READ, FALSE, manifestName.c_str());
                if (hManifest) {
                    BroadcastManifest* pView = (BroadcastManifest*)MapViewOfFile(hManifest, FILE_MAP_READ, 0, 0, sizeof(BroadcastManifest));
                    if (pView) {
                        if (memcmp(&pView->adapterLuid, &pImpl->m_adapterLuid, sizeof(LUID)) == 0) {
                            DiscoveredSharedStream stream;
                            stream.processId = pe32.th32ProcessID;
                            stream.processName = pe32.szExeFile;
                            stream.producerType = sig.second;
                            stream.manifestName = manifestName;
                            stream.textureName = pView->textureName;
                            stream.fenceName = pView->fenceName;
                            stream.adapterLuid = pView->adapterLuid;
                            pImpl->m_discoveredStreams.push_back(stream);
                        }
                        UnmapViewOfFile(pView);
                    }
                    CloseHandle(hManifest);
                    break;
                }
            }
        } while (Process32NextW(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
}

const std::vector<DiscoveredSharedStream>& Discovery::GetDiscoveredStreams() const { 
    return pImpl->m_discoveredStreams; 
}

}