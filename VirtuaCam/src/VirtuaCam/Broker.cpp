#define WIN32_LEAN_AND_MEAN
#include "pch.h"
#include <wrl.h>
#include <memory>
#include "App.h"
#include "Utilities.h"
#include "Multiplexer.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using namespace Microsoft::WRL;

#define BROKER_API __declspec(dllexport)

static ComPtr<ID3D11Device>           g_device;
static std::unique_ptr<Multiplexer>   g_multiplexer;
static BrokerState                    g_brokerState = BrokerState::Searching;
static bool                           g_isInitialized = false;

extern "C" {
    BROKER_API void InitializeBroker(ID3D11Device* pDevice) {
        if (g_isInitialized) return;
        if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return;

        g_device = pDevice;

        g_multiplexer = std::make_unique<Multiplexer>();
        if (FAILED(g_multiplexer->Initialize(g_device.Get())))
        {
            g_multiplexer.reset();
        }
        g_isInitialized = true;
    }

    BROKER_API void ShutdownBroker() {
        if (!g_isInitialized) return;
        if (g_multiplexer)
        {
            g_multiplexer->Shutdown();
            g_multiplexer.reset();
        }
        g_device.Reset();
        CoUninitialize();
        g_isInitialized = false;
    }
    
    BROKER_API void UpdateProducerPriorityList(const DWORD* pids, int count) {
        if (g_multiplexer)
        {
            g_multiplexer->UpdateProducerPriorityList(pids, count);
        }
    }

    BROKER_API void SetPreferredProducerPID(DWORD pid) {
        if (g_multiplexer)
        {
            g_multiplexer->SetPreferredProducerPID(pid);
        }
    }

    BROKER_API void SetPipProducerPID(DWORD pid) {
        if (g_multiplexer)
        {
            g_multiplexer->SetPipProducerPID(pid);
        }
    }

    BROKER_API void RenderBrokerFrame() {
        if (!g_multiplexer)
        {
            g_brokerState = BrokerState::Failed;
            return;
        }
        
        g_multiplexer->DiscoverAndManageConnections();
        if (SUCCEEDED(g_multiplexer->Composite()))
        {
            g_brokerState = BrokerState::Connected;
        }
        else
        {
            g_brokerState = BrokerState::Searching;
        }
    }

    BROKER_API HANDLE GetSharedTextureHandle() {
        if (g_multiplexer)
        {
            return g_multiplexer->GetSharedOutputHandle();
        }
        return nullptr;
    }

    BROKER_API BrokerState GetBrokerState() {
        return g_brokerState;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(hModule);
    return TRUE;
}