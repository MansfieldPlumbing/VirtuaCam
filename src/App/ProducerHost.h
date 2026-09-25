// =============================================================================
// ProducerHost.h  --  Starts and stops producer processes
// =============================================================================
// Producers are this same executable started with `--producer <kind> <arg>`.
// They are reference counted by what they produce, so the same webcam used
// in two slots is opened once.  All of them live in a kill-on-close job, so
// they can never outlive the tray app, and each process handle is watched
// with a thread-pool wait that posts `exitedMessage` (wParam = pid) to the
// tray window -- no polling for liveness.
// =============================================================================

#pragma once

#include <windows.h>
#include <wil/resource.h>
#include <memory>
#include <string>
#include <vector>

struct ProducerSpec {
    std::wstring kind;       // camera | window | display | whiteboard
    std::wstring argument;
    bool operator==(const ProducerSpec& other) const { return kind == other.kind && argument == other.argument; }
};

class ProducerHost {
public:
    ProducerHost();
    ProducerHost(const ProducerHost&) = delete;
    ProducerHost& operator=(const ProducerHost&) = delete;
    ~ProducerHost();

    HRESULT Initialize(HWND notifyWindow, UINT exitedMessage);

    // Starts the producer (or adds a reference to a running one); returns its pid.
    HRESULT Acquire(const ProducerSpec& spec, DWORD* pid);
    void Release(const ProducerSpec& spec);
    void StopAll();

    // Handles exitedMessage: forgets the process and reports what it was.
    // Returns false for processes that were stopped on purpose.
    bool OnExited(DWORD pid, ProducerSpec* spec, DWORD* exitCode);

    bool Owns(DWORD pid) const;

private:
    struct Process;
    void Stop(Process& process);

    HWND m_notifyWindow = nullptr;
    UINT m_exitedMessage = 0;
    wil::unique_handle m_job;
    std::vector<std::unique_ptr<Process>> m_processes;
};
