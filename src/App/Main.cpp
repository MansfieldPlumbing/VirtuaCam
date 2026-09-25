// =============================================================================
// Main.cpp  --  VirtuaCam.exe entry point
// =============================================================================
//   VirtuaCam.exe                          tray application
//   VirtuaCam.exe --producer <kind> <arg>  producer process (see Producers.h)
// =============================================================================

#include "Producers.h"
#include "TrayApp.h"

#include <mfapi.h>
#include <shellapi.h>

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int argc = 0;
    wil::unique_hlocal_ptr<LPWSTR[]> argv(CommandLineToArgvW(GetCommandLineW(), &argc));
    if (argv && argc >= 3 && wcscmp(argv[1], L"--producer") == 0)
        return RunProducer(instance, argv[2], argc >= 4 ? argv[3] : L"");

    // One tray instance per user session.
    wil::unique_mutex_nothrow instanceLock(CreateMutexW(nullptr, FALSE, L"Local\\VirtuaCam.Tray"));
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

    auto com = wil::CoInitializeEx_failfast(COINIT_APARTMENTTHREADED);
    if (FAILED(MFStartup(MF_VERSION)))
        return 1;
    TrayApp app;
    const int code = app.Run(instance);
    MFShutdown();
    return code;
}
