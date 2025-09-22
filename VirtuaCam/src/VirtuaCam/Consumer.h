// --- src/DirectPortVirtuaCam/Consumer.h ---
#pragma once
#include <windows.h>
#include <d3d11.h>

#ifdef CONSUMER_EXPORTS
#define CONSUMER_API __declspec(dllexport)
#else
#define CONSUMER_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Initializes the consumer node, connecting to the specified upstream producer PID.
CONSUMER_API HRESULT InitializeProducer(DWORD inputProducerPid, const wchar_t* manifestPrefix);

// Contains the main processing loop. This function will block.
CONSUMER_API void RunProducer();

// Shuts down the consumer node and releases all resources.
CONSUMER_API void ShutdownProducer();

#ifdef __cplusplus
}
#endif
