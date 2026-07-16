# Running VirtuaCam as a Windows Service

Status: **design proposal** — nothing in this document is implemented yet.

## Motivation

Today `VirtuaCam.exe` is a tray app: the virtual camera exists only while the
user runs it (and it must run elevated). A service would give:

* camera available immediately at boot/logon, before any UI runs,
* survives logoff/logon of the interactive user,
* no UAC prompt at every start (the service is installed once, elevated).

## The hard constraints (read this first)

A naive "run VirtuaCam.exe as a service" does **not** work. Three subsystems
in the current process are tied to the *interactive user session*, and a
service runs in **session 0**:

1. **`MFCreateVirtualCamera`** (`App.cpp` → `RegisterVirtualCamera()`):
   created with `MFVirtualCameraLifetime_Session` and
   `MFVirtualCameraAccess_CurrentUser`. A session-0 service would have to use
   `MFVirtualCameraLifetime_System` / `MFVirtualCameraAccess_AllUsers`, which
   is supported by the API precisely for this scenario — but the call must
   then be made from the service, not from the tray app.
2. **Windows.Graphics.Capture** (`MFGraphicsCapture.cpp`): can only capture
   windows/monitors of an interactive session. Capture producers **must**
   keep running in the user's session.
3. **Tray icon / menus / preview** (`UI.cpp`, `Menu.cpp`): UI cannot live in
   session 0 at all.

So the correct shape is a **split**: a small service owning camera lifetime +
broker, and the existing app demoted to a non-elevated, per-user controller.

## Proposed architecture

```
┌────────────────────────── session 0 ─────────────────────────────┐
│  VirtuaCamSvc.exe  (new, ~400 lines)                             │
│   • SCM boilerplate (SERVICE_WIN32_OWN_PROCESS, stop/shutdown)   │
│   • MFCreateVirtualCamera(System lifetime, AllUsers access)      │
│   • hosts DirectPortBroker.dll frame loop (currently in OnIdle)  │
│   • control pipe: \\.\pipe\VirtuaCamControl  (source selection,  │
│     PIP layout, compositing mode — i.e. today's InformBroker())  │
└───────────────────────────────────────────────────────────────────┘
            ▲ named pipe (commands)        ▲ shared textures (frames)
┌────────────────────────── user session ──────────────────────────┐
│  VirtuaCam.exe  (existing tray app, minus elevation requirement) │
│   • tray UI, menus, preview window (unchanged)                   │
│   • launches VirtuaCamProcess.exe producers (unchanged — they    │
│     must stay in-session for Windows.Graphics.Capture)           │
│   • sends selections over the pipe instead of calling broker     │
│     function pointers directly                                   │
└───────────────────────────────────────────────────────────────────┘
```

What makes this cheap for *this* codebase: frames already cross process
boundaries via named shared textures/fences with a `Global\` namespace and an
explicit SDDL DACL (`D:P(A;;GA;;;AU)` — see `MFGraphicsCapture.cpp`,
`MFCamera.cpp`, `Consumer.cpp`). Producers in the user session and a broker
in session 0 can already see each other's objects. **Only the command path**
(`App.cpp`'s direct `GetProcAddress` calls into `DirectPortBroker.dll`:
`UpdateProducerPriorityList`, `SetCompositingMode`, `GetBrokerState`) needs
to become an IPC message, and it is already a flat, five-integers-style API —
trivially serialisable over a named pipe.

### Work items

1. **Extract broker hosting** from `App.cpp` (`LoadBroker`, `OnIdle`,
   `InformBroker`) behind a small interface so it can be hosted either
   in-proc (today's mode) or in the service.
2. **`VirtuaCamSvc.exe`**: `StartServiceCtrlDispatcher`, `RegisterServiceCtrlHandlerEx`,
   pump = `RenderBrokerFrame()` loop; pipe server with a DACL allowing the
   interactive user; `MFCreateVirtualCamera(..., MFVirtualCameraLifetime_System,
   MFVirtualCameraAccess_AllUsers, ...)`.
3. **Tray app changes**: detect the service (`OpenService`); if present, drop
   the `IsRunningAsAdmin()` gate and route `InformBroker()` through the pipe.
   If absent, run exactly as today (keep the standalone mode — it is the
   best debugging environment).
4. **Install/uninstall**: `sc create VirtuaCamSvc binPath= ... start= auto`
   (or `CreateService`) wired into `build.ps1` as `-InstallService` /
   `-UninstallService`.

### Gotchas to plan for

* `DllRegisterServer` writes `HKLM` CLSID keys for `DirectPortClient.dll` —
  unchanged, still a one-time admin step.
* The frame-server loads `DirectPortClient.dll` into *its* process per
  consuming app; that DLL connects to the broker's shared texture by name —
  the broker's output objects must stay in the `Global\` namespace (they
  already are: `Global\VirtuaCast_Broker_Texture`, see `UI.cpp`).
* Service must handle "no producers yet" gracefully forever — the existing
  `BrokerState::Searching` path already does.
* Add a `SERVICE_CONTROL_SESSIONCHANGE` handler if per-session behaviour is
  ever needed (e.g. pausing compositing when no user is logged on).

## Recommendation

Do this *after* the planned UI/IPC seam extraction, and keep the dual-mode
(standalone tray vs service+tray) permanently. The split is the precondition
for the virtual-display-driver milestone too — the service is the natural
owner of a driver-fed producer that exists independently of any user session.
