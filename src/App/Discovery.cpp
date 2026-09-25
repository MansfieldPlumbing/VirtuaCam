// =============================================================================
// Discovery.cpp
// =============================================================================

#include "Discovery.h"
#include "SharedFrame.h"

#include <tlhelp32.h>

std::vector<DiscoveredProducer> DiscoverProducers(const LUID& adapter)
{
    std::vector<DiscoveredProducer> found;
    wil::unique_handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot || snapshot.get() == INVALID_HANDLE_VALUE)
        return found;

    PROCESSENTRY32W entry{ sizeof(entry) };
    for (BOOL more = Process32FirstW(snapshot.get(), &entry); more; more = Process32NextW(snapshot.get(), &entry)) {
        wil::unique_handle mapping(OpenFileMappingW(FILE_MAP_READ, FALSE, Ipc::ProducerManifestName(entry.th32ProcessID).c_str()));
        if (!mapping)
            continue;
        auto* manifest = static_cast<const Ipc::BroadcastManifest*>(
            MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0, sizeof(Ipc::BroadcastManifest)));
        if (!manifest)
            continue;
        if (manifest->adapterLuid.LowPart == adapter.LowPart && manifest->adapterLuid.HighPart == adapter.HighPart)
            found.push_back({ entry.th32ProcessID, entry.szExeFile, manifest->width, manifest->height });
        UnmapViewOfFile(manifest);
    }
    return found;
}
