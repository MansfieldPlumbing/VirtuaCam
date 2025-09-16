#pragma once

#include <windows.h>

#ifdef CAMERAMODULE_EXPORTS
#define CAMERAMODULE_API __declspec(dllexport)
#else
#define CAMERAMODULE_API __declspec(dllimport)
#endif

CAMERAMODULE_API HRESULT InitializeCamera();
CAMERAMODULE_API void ShutdownCamera();
CAMERAMODULE_API void CycleCameraSource();
CAMERAMODULE_API void ToggleMirrorStream();