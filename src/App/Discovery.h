// =============================================================================
// Discovery.h  --  Finds shared-frame producers running on this machine
// =============================================================================
// A producer is any process that has published
// "DirectPort_Producer_Manifest_<pid>" (see SharedFrame.h).  Scanning walks
// the process list and probes for that mapping, so it is only done on demand:
// when the tray menu opens and, in grid mode, every couple of seconds.
// =============================================================================

#pragma once

#include <windows.h>
#include <string>
#include <vector>

struct DiscoveredProducer {
    DWORD pid = 0;
    std::wstring processName;
    UINT width = 0;
    UINT height = 0;
};

// Producers on the given adapter (shared textures cannot cross adapters).
std::vector<DiscoveredProducer> DiscoverProducers(const LUID& adapter);
