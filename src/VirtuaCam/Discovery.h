// =============================================================================
// Discovery.h  --  Producer discovery via named shared-memory manifests
// =============================================================================
// Each producer process (camera, screen capture, consumer) advertises itself
// by creating a Windows named file-mapping called:
//
//   DirectPort_Producer_Manifest_<PID>
//
// The mapping contains a BroadcastManifest struct (defined in App.h/Broker.cpp)
// that holds the names of the shared texture and fence the producer has
// created, along with the GPU adapter LUID it is running on.
//
// Discovery::DiscoverStreams() snapshots all running processes and probes each
// one for this mapping.  Only producers running on the same GPU adapter as the
// caller (matched by LUID) are returned; cross-adapter zero-copy texture
// sharing is not possible.
// =============================================================================

#pragma once

#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <memory>

struct ID3D11Device;

namespace VirtuaCam {

// ---------------------------------------------------------------------------
// DiscoveredSharedStream
// ---------------------------------------------------------------------------
// Describes a single live producer discovered on the system.

struct DiscoveredSharedStream {
    DWORD        processId;     // PID of the producer process
    std::wstring processName;   // Executable name (e.g. "VirtuaCamProcess.exe")
    std::wstring producerType;  // Category string (e.g. "DirectPort")
    std::wstring manifestName;  // Full name of the producer's manifest file-mapping
    std::wstring textureName;   // Name of the shared D3D11 texture NT handle
    std::wstring fenceName;     // Name of the shared D3D11 fence NT handle
    LUID         adapterLuid;   // GPU adapter the producer is running on
};

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------
// One-shot stream scanner.  Call DiscoverStreams() each frame (or on demand)
// and read the results from GetDiscoveredStreams().

class Discovery {
public:
    Discovery();
    ~Discovery();

    // Store the D3D11 device used to identify the local GPU adapter LUID.
    // Must be called once before DiscoverStreams().
    HRESULT Initialize(ID3D11Device* device);

    // Release the D3D11 device reference.
    void Teardown();

    // Snapshot all running processes and populate the discovered-stream list.
    // Results are cleared and rebuilt on every call.
    void DiscoverStreams();

    // Access the most recently discovered streams.
    const std::vector<DiscoveredSharedStream>& GetDiscoveredStreams() const;

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

}
