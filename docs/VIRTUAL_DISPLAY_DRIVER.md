# Integrating a User-Mode Virtual Display Driver (IddCx)

Status: **design proposal** — nothing in this document is implemented yet.

## What it is

An *Indirect Display Driver* (IDD) is a user-mode driver, built on Microsoft's
**IddCx** (Indirect Display Driver Class eXtension) framework, that makes
Windows believe a real monitor is attached. The OS extends the desktop onto
it, applications can be moved there, and the GPU composites it like any other
monitor — but the "monitor" delivers its swap-chain buffers to *our* code
instead of to a cable.

This is the same mechanism used by commercial tools (e.g. wired/wireless
display extenders, virtual-monitor apps for tablets-as-second-screen).

## Why it fits VirtuaCam unusually well

VirtuaCam's whole pipeline is built around one contract, defined in
`src/VirtuaCam/Tools.h`:

```
BroadcastManifest  (memory-mapped file: DirectPort_Producer_Manifest_<pid>)
  ├─ width / height / DXGI_FORMAT / adapter LUID
  ├─ textureName  → named shared ID3D11Texture2D (D3D11_RESOURCE_MISC_SHARED_NTHANDLE)
  └─ fenceName    → named shared ID3D11Fence + frameValue for synchronisation
```

Anything that publishes this manifest *is* a producer — the broker
(`Broker.cpp` / `Multiplexer.cpp`) discovers it automatically via
`Discovery.cpp` and composites it on the GPU with zero copies.

An IddCx driver hands us exactly what a producer needs: every time the
desktop on the virtual monitor changes, IddCx gives the driver a
`IDXGIResource`/`ID3D11Texture2D` for the new frame
(`IddCxSwapChainReleaseAndAcquireBuffer`). The integration is therefore:

```
[Virtual Monitor (IddCx driver)] → acquire buffer → CopyResource into shared
texture → Signal fence → update manifest      …and the broker does the rest.
```

A new producer type — "Virtual Display" — that gives users a monitor they can
drag *anything* onto (full-screen games, PowerPoint in presenter mode, apps
that resist window capture) and it shows up in Zoom/Teams as the camera feed.
No window-picker, no occlusion problems, no DWM rounded corners.

## Proposed architecture

Two new components, mirroring the existing producer pattern:

| Component | Kind | Role |
|---|---|---|
| `VirtuaCamDisplay.dll` + `.inf` | UMDF 2 / IddCx driver | The virtual monitor itself. Runs in a UMDF host process under `WUDFHost.exe`. |
| `DirectPortDisplay.dll` | producer DLL (like `MFCamera.cpp`) | Loaded by `VirtuaCamProcess.exe --type display`. Bridges driver frames to a `BroadcastManifest`. |

The driver and the producer DLL share frames the same way the rest of the
system already does — a named shared texture + fence — so the producer side
is ~200 lines and identical in shape to `MFGraphicsCapture.cpp`.

Driver-side responsibilities (the genuinely new code):

1. `IddCxDeviceInitConfig` / `IddCxAdapterInitAsync` — create the virtual
   adapter at driver start.
2. `IddCxMonitorCreate` + a hard-coded EDID blob — plug in "VirtuaCam
   Display" (advertise 1920×1080@60 and 1280×720@30/60 to match
   `Formats.cpp`).
3. `EvtIddCxMonitorAssignSwapChain` — start a frame-pump thread that loops
   `IddCxSwapChainReleaseAndAcquireBuffer`, copies into the shared texture,
   signals the fence, updates `frameValue` in the manifest.
4. `EvtIddCxMonitorUnassignSwapChain` — stop the pump.

UI integration is one new entry in the source menus (`UI.cpp`
`BuildSourceSubMenu`): **"Virtual Display"**, which launches
`VirtuaCamProcess.exe --type display` exactly like the camera/window items,
plus driver install/uninstall handled by `build.ps1` (`pnputil /add-driver`).

## Costs and constraints (why this is a separate milestone)

* **Toolchain**: requires the Windows Driver Kit (WDK) and the WDK VS
  extension on top of the current prerequisites. CMake can build UMDF
  drivers but it is friction; a small `.vcxproj` just for the driver may be
  pragmatic.
* **Signing**: drivers must be signed. Local development works with
  test-signing mode (`bcdedit /set testsigning on`); distributing to other
  machines requires an EV certificate + Microsoft attestation signing. This
  is the single biggest practical hurdle.
* **OS support**: IddCx 1.4+ (Windows 10 1903) is fine for everything above;
  HDR/advanced features need newer IddCx versions.
* **Installation is system-wide** and needs admin (the app already requires
  admin, so no regression).

## Recommended sequencing

1. Keep the manifest contract frozen (it is the integration boundary).
2. Prototype the driver standalone from the Microsoft `IndirectDisplay`
   sample (`Windows-driver-samples/video/IndirectDisplay`), modified only to
   write into a named shared texture + fence.
3. Add `DirectPortDisplay.dll` producer + menu item (small, low-risk).
4. Only then deal with signing/packaging.
