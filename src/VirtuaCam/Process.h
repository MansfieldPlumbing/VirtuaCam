#pragma once
#include <windows.h>
#include <string>

typedef HRESULT (*PFN_InitializeProducer)(const WCHAR* args);
typedef void (*PFN_ProcessFrame)();
typedef void (*PFN_ShutdownProducer)();

struct ProducerModule {
    HMODULE hModule = nullptr;
    PFN_InitializeProducer Initialize = nullptr;
    PFN_ProcessFrame Process = nullptr;
    PFN_ShutdownProducer Shutdown = nullptr;
};

bool ParseCommandLine(const WCHAR* cmdLine, std::wstring& type, std::wstring& args);
void LoadProducerModule(const std::wstring& type, ProducerModule& module);