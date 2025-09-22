// --- src/DirectPortVirtuaCam/Multiplexer.h ---
#pragma once

#include <vector>
#include <memory>
#include "Utilities.h"

class Multiplexer
{
public:
    Multiplexer();
    ~Multiplexer();

    HRESULT Initialize(ID3D11Device* device);
    void Shutdown();
    void DiscoverAndManageConnections();
    HRESULT Composite();
    ID3D11ShaderResourceView* GetOutputSRV();
    HANDLE GetSharedOutputHandle();
    void UpdateProducerPriorityList(const DWORD* pids, int count);
    void SetPreferredProducerPID(DWORD pid);
    void SetPipProducerPID(DWORD pid);

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};