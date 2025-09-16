// File: src/DirectPortVirtuaCam/Consumer.h

#pragma once

#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace VirtuaCam {

    struct DiscoveredSharedStream {
        DWORD processId;
        std::wstring processName;
        std::wstring producerType;
        std::wstring manifestName;
        std::wstring textureName;
        std::wstring fenceName;
        LUID adapterLuid;
    };

    class Consumer {
    public:
        Consumer();
        ~Consumer();

        HRESULT Initialize(ID3D11Device* device);
        void Teardown();

        void DiscoverStreams();
        
        const std::vector<DiscoveredSharedStream>& GetDiscoveredStreams() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> pImpl;
    };

}