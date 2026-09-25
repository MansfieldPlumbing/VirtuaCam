// =============================================================================
// ProducerHost.cpp
// =============================================================================

#include "ProducerHost.h"

#include <algorithm>

struct ProducerHost::Process {
    ProducerSpec spec;
    int references = 0;
    wil::unique_process_information info;
    HANDLE wait = nullptr;
    HWND notifyWindow = nullptr;
    UINT exitedMessage = 0;
    bool stopping = false;
};

ProducerHost::ProducerHost() = default;

ProducerHost::~ProducerHost()
{
    StopAll();
}

HRESULT ProducerHost::Initialize(HWND notifyWindow, UINT exitedMessage)
{
    m_notifyWindow = notifyWindow;
    m_exitedMessage = exitedMessage;
    m_job.reset(CreateJobObjectW(nullptr, nullptr));
    RETURN_LAST_ERROR_IF_NULL(m_job.get());
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    RETURN_IF_WIN32_BOOL_FALSE(SetInformationJobObject(m_job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)));
    return S_OK;
}

bool ProducerHost::Owns(DWORD pid) const
{
    return std::any_of(m_processes.begin(), m_processes.end(), [pid](const auto& p) { return p->info.dwProcessId == pid; });
}

HRESULT ProducerHost::Acquire(const ProducerSpec& spec, DWORD* pid)
{
    *pid = 0;
    for (auto& process : m_processes) {
        if (process->spec == spec && !process->stopping) {
            process->references++;
            *pid = process->info.dwProcessId;
            return S_OK;
        }
    }

    wchar_t exe[MAX_PATH]{};
    RETURN_LAST_ERROR_IF(!GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe)));
    std::wstring commandLine = L"\"" + std::wstring(exe) + L"\" --producer " + spec.kind;
    if (!spec.argument.empty())
        commandLine += L" \"" + spec.argument + L"\"";

    auto process = std::make_unique<Process>();
    process->spec = spec;
    process->references = 1;
    process->notifyWindow = m_notifyWindow;
    process->exitedMessage = m_exitedMessage;

    STARTUPINFOW startup{ sizeof(startup) };
    RETURN_IF_WIN32_BOOL_FALSE(CreateProcessW(exe, commandLine.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                                              nullptr, nullptr, &startup, &process->info));
    if (m_job)
        AssignProcessToJobObject(m_job.get(), process->info.hProcess);
    ResumeThread(process->info.hThread);

    RegisterWaitForSingleObject(&process->wait, process->info.hProcess, [](PVOID context, BOOLEAN) {
        auto* p = static_cast<Process*>(context);
        PostMessageW(p->notifyWindow, p->exitedMessage, p->info.dwProcessId, 0);
    }, process.get(), INFINITE, WT_EXECUTEONLYONCE);

    *pid = process->info.dwProcessId;
    m_processes.push_back(std::move(process));
    return S_OK;
}

void ProducerHost::Stop(Process& process)
{
    process.stopping = true;
    // Producers quit cleanly on WM_QUIT (releasing cameras); force only if stuck.
    PostThreadMessageW(process.info.dwThreadId, WM_QUIT, 0, 0);
    if (WaitForSingleObject(process.info.hProcess, 2000) == WAIT_TIMEOUT)
        TerminateProcess(process.info.hProcess, 1);
    if (process.wait) {
        UnregisterWaitEx(process.wait, INVALID_HANDLE_VALUE);   // waits for a running callback
        process.wait = nullptr;
    }
}

void ProducerHost::Release(const ProducerSpec& spec)
{
    for (auto it = m_processes.begin(); it != m_processes.end(); ++it) {
        if ((*it)->spec == spec && !(*it)->stopping) {
            if (--(*it)->references > 0)
                return;
            Stop(**it);
            m_processes.erase(it);
            return;
        }
    }
}

void ProducerHost::StopAll()
{
    for (auto& process : m_processes)
        Stop(*process);
    m_processes.clear();
}

bool ProducerHost::OnExited(DWORD pid, ProducerSpec* spec, DWORD* exitCode)
{
    for (auto it = m_processes.begin(); it != m_processes.end(); ++it) {
        Process& process = **it;
        if (process.info.dwProcessId != pid)
            continue;
        if (process.wait) {
            UnregisterWaitEx(process.wait, INVALID_HANDLE_VALUE);
            process.wait = nullptr;
        }
        *exitCode = 0;
        GetExitCodeProcess(process.info.hProcess, exitCode);
        *spec = process.spec;
        m_processes.erase(it);
        return true;
    }
    return false;
}
