#pragma once
#include <windows.h>

#ifdef PRODUCER_EXPORTS
#define PRODUCER_API __declspec(dllexport)
#else
#define PRODUCER_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

PRODUCER_API HRESULT InitializeProducer(const wchar_t* args);
PRODUCER_API void ProcessFrame();
PRODUCER_API void ShutdownProducer();

#ifdef __cplusplus
}
#endif