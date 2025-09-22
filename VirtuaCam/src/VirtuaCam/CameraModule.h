// --- src/DirectPortVirtuaCam/CameraModule.h ---
#pragma once
#include <windows.h>
#include <d3d11.h>

#ifdef CAMERAMODULE_EXPORTS
#define CAMERAMODULE_API __declspec(dllexport)
#else
#define CAMERAMODULE_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

CAMERAMODULE_API HRESULT InitializeProducer(int cameraIndex, const wchar_t* manifestPrefix);
CAMERAMODULE_API void RunProducer();
CAMERAMODULE_API void ShutdownProducer();

#ifdef __cplusplus
}
#endif
