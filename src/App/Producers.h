// =============================================================================
// Producers.h  --  Built-in frame producers
// =============================================================================
// VirtuaCam.exe doubles as its own producer host: the tray app starts
//
//   VirtuaCam.exe --producer camera     <symbolic link>
//   VirtuaCam.exe --producer window     <hwnd>
//   VirtuaCam.exe --producer display    <device name, e.g. \\.\DISPLAY2>
//   VirtuaCam.exe --producer whiteboard
//
// Each runs in its own process (a crashing camera driver cannot take the
// tray down) and publishes frames through Ipc::FramePublisher, exactly like
// any third-party producer would.  All of them are event driven: frames are
// pushed from capture callbacks or user input, and the process otherwise
// sleeps in GetMessage.  The tray app stops a producer by posting WM_QUIT to
// its main thread.
// =============================================================================

#pragma once

#include <windows.h>
#include <string>

enum ProducerExitCode : int {
    ProducerExitOk = 0,
    ProducerExitBadArguments = 2,
    ProducerExitStartFailed = 3,
    ProducerExitSourceLost = 4,
};

int RunProducer(HINSTANCE instance, const std::wstring& kind, const std::wstring& argument);

int RunCameraProducer(const std::wstring& symbolicLink);
int RunWindowCaptureProducer(HWND window);
int RunDisplayCaptureProducer(const std::wstring& deviceName);
int RunWhiteboardProducer(HINSTANCE instance);

// Pumps messages until WM_QUIT; returns its exit code.
int RunMessageLoop();
