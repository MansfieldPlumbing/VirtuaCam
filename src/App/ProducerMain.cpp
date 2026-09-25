// =============================================================================
// ProducerMain.cpp  --  Dispatch for `VirtuaCam.exe --producer <kind> <arg>`
// =============================================================================

#include "Producers.h"

#include <roapi.h>
#include <wil/resource.h>

int RunMessageLoop()
{
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return (int)message.wParam;
}

int RunProducer(HINSTANCE instance, const std::wstring& kind, const std::wstring& argument)
{
    // Capture callbacks arrive on MTA thread-pool threads; the main thread
    // only pumps messages, so it joins the MTA too.
    auto apartment = wil::RoInitialize_failfast(RO_INIT_MULTITHREADED);

    if (kind == L"camera")
        return RunCameraProducer(argument);
    if (kind == L"window")
        return RunWindowCaptureProducer(reinterpret_cast<HWND>(static_cast<ULONG_PTR>(_wcstoui64(argument.c_str(), nullptr, 10))));
    if (kind == L"display")
        return RunDisplayCaptureProducer(argument);
    if (kind == L"whiteboard")
        return RunWhiteboardProducer(instance);
    return ProducerExitBadArguments;
}
