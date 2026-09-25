// =============================================================================
// Settings.h  --  Per-user preferences (HKCU\Software\VirtuaCam)
// =============================================================================

#pragma once

#include <windows.h>

struct Settings {
    UINT outputWidth;
    UINT outputHeight;
    UINT outputFps;

    static Settings Load();
    void Save() const;
};

// "Start with Windows" (HKCU Run key).
bool IsAutostartEnabled();
void SetAutostartEnabled(bool enabled);
