# VirtuaCam

![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg) ![Platform: Windows 11](https://img.shields.io/badge/Platform-Windows_11-blue.svg) ![Language: C++20](https://img.shields.io/badge/Language-C++20-orange.svg)

VirtuaCam is a virtual webcam for Windows 11 that shows whatever other programs hand it on the GPU. Pick a physical camera, a window, a whole display or the built-in whiteboard (or any program that speaks its shared-frame protocol). VirtuaCam composites your choices, with optional picture-in-picture corners or an automatic grid. Zoom, Teams, OBS, Discord, the Camera app and anything else that uses Media Foundation see the result as a normal camera called **VirtuaCam**.

![demo](https://github.com/user-attachments/assets/c99f8c50-8b2d-4b98-bb63-fc0e57082d44)

## How it works

```
 producers (any process)          VirtuaCam.exe                     Camera Frame Server
 ─────────────────────────        ───────────────────────────       ──────────────────────────
 webcam  ─┐                       Broker (own thread)               VirtuaCamSource.dll
 window  ─┤  shared texture       ├ waits on producer fences  ──►   scales + converts the
 display ─┼─ + shared fence  ──►  ├ composites layers (GPU)         broker frame into the
 whiteboard┤  + manifest          └ publishes one shared frame      format/size each app asks
 your app ─┘                                                        for (NV12 / YUY2 / RGB32)
```

* **Frames never leave the GPU.** Every hop is a D3D11 shared texture synchronised by a shared fence. The only CPU copy happens when a consuming app refuses GPU samples.
* **Nothing polls.** The broker sleeps until a producer's fence signals, a producer exits, or you change the layout. Output is capped at the chosen frame rate, and a frame that hasn't changed isn't redrawn. Producers push frames from capture callbacks. The tray UI sleeps in `GetMessage`.
* **Any size, any rate.** The camera offers 640×360 through 3840×2160 at 15–60 fps in NV12, YUY2 and RGB32. Whatever an app negotiates is produced by a single D3D11 video-processor blit (scale, letterbox, colour conversion) straight into the app's sample.
* **One protocol for everything.** Built-in and third-party producers use the same contract ([`src/Common/SharedFrame.h`](src/Common/SharedFrame.h)), so anything that publishes a frame appears in the tray menu automatically.

## Build

Requirements: Windows 11, Visual Studio 2022 with *Desktop development with C++*, CMake 3.21 or newer. The WIL headers are fetched automatically.

```powershell
.\build.ps1                 # -> build\bin\Release\VirtuaCam.exe, VirtuaCamSource.dll
.\build.ps1 -Register       # also registers the camera DLL (one UAC prompt)
.\build.ps1 -Installer      # also builds installer\Output\VirtuaCam-*-Setup.exe (Inno Setup 6)
```

The camera DLL has to be registered machine-wide once, because the Camera Frame Server only reads HKLM. The installer does this. If you run a bare build, VirtuaCam offers to do it on first start. After that, VirtuaCam runs as a normal user.

## Use

Run `VirtuaCam.exe` and click the tray icon.

| Menu | |
|---|---|
| thumbnail / **Open preview** | live view of exactly what the camera sends (double-click the icon too) |
| **Source** | the full-frame source: a camera, a display, a window, the whiteboard, another app's producer, or **All sources (grid)** |
| **Picture in picture** | up to four corner overlays, each with the same choices |
| **Output** | composite resolution (720p–4K) and frame rate (30/60) |
| **Start with Windows** | adds or removes the per-user Run entry |

Each built-in source runs in its own process (`VirtuaCam.exe --producer …`). A misbehaving camera driver can't take the tray app down, and all producers exit with it.

**Whiteboard.** Choose *Whiteboard* as the source and a window opens. Draw with the mouse or a pen and the strokes go straight to the camera. Right-click clears, `1`–`6` pick colours (6 erases), `[` `]` change the brush size, and `Ctrl+Z` undoes. Put your webcam in a picture-in-picture corner to sketch while you talk.

**Virtual displays.** *Source → Display* captures any monitor Windows knows about, including virtual monitors created by indirect-display drivers. VirtuaCam doesn't ship a driver. To create an extra monitor, use an IddCx virtual-display driver of your choice, then pick that display in VirtuaCam: drag anything onto it and it's on camera.

## Writing a producer

A producer is any process that publishes:

1. a named memory-mapped `BroadcastManifest` called `DirectPort_Producer_Manifest_<pid>`,
2. a named shared `ID3D11Texture2D` (`D3D11_RESOURCE_MISC_SHARED_NTHANDLE`) and a named shared `ID3D11Fence`, whose names are stored in the manifest.

For each frame: render into the texture, `Signal` the fence with the next value, then store that value in `manifest.frameValue`. The layout and naming rules are in [`SharedFrame.h`](src/Common/SharedFrame.h). If you build against this repository, `Ipc::FramePublisher` does all of it: `OpenForProcess(device, w, h)`, draw into `Texture()`, then `Publish(context)`.

## Layout

```
src/Common   shared-frame IPC, D3D11 helpers (linked into both binaries)
src/Camera   VirtuaCamSource.dll — the Media Foundation virtual camera
src/App      VirtuaCam.exe — tray UI, broker/compositor, built-in producers
installer    Inno Setup script
tools        generator for the "NO SIGNAL" wordmark mask
```

## License

MIT. See [LICENSE](LICENSE).
