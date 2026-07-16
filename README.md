# VirtuaCam

![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)![Platform: Windows 11](https://img.shields.io/badge/Platform-Windows_11-blue.svg)![Language: C++20](https://img.shields.io/badge/Language-C++20-orange.svg)

VirtuaCam is a modern, high-performance virtual camera for Windows built with a decoupled producer-consumer architecture. It enables low-latency, zero-copy video injection from external DirectX applications, games, or other video sources, exposing them as a standard webcam on your system for use in applications like Zoom, Microsoft Teams, OBS, Discord, and more.

![ezgif-81ee43ea485d78](https://github.com/user-attachments/assets/c99f8c50-8b2d-4b98-bb63-fc0e57082d44)

## Core Concept: A High-Performance Video Broker

Unlike traditional virtual cameras that generate their own content, VirtuaCam acts as a high-performance transport system—a "broker"—that discovers and composites video feeds from other applications (producers). This is achieved directly on the GPU, avoiding costly memory transfers between the CPU and GPU, which results in minimal performance impact.

The data flow is designed for efficiency:

`[Your App (Producer)]` ---> `[Shared D3D11 Texture & Fence]` ---> `[VirtuaCam Broker (Consumer)]` ---> `[Zoom, Teams, etc.]`

This architecture is ideal for applications like game streaming, creative coding, real-time video filters, screen sharing, or any scenario where you need to pipe a custom, hardware-accelerated video stream into a standard camera feed.

## Architecture Overview

### Virtual Object Manager (VOM) Pattern

VirtuaCam implements a **Virtual Object Manager** pattern inspired by kernel-mode handle tables, providing:

- **Deterministic Resource Lifecycle**: All resources (menus, textures, IPC handles) are tracked via generational handle tables with explicit reference counting
- **Thread-Safe Operations**: Handle tables use critical sections to prevent race conditions between UI thread, broker thread, and producer processes
- **Graceful Teardown**: Resources can be reclaimed deterministically via `DropPrefix()` and `Terminate()` operations, preventing leaks and deadlocks
- **Generational Handles**: Stale handle IDs are rejected O(1), preventing use-after-free bugs

### Inter-Process Communication (IPC) Design

**Key Insight**: Uses `Local\` namespace instead of `Global\` for shared memory and texture handles.

**Why `Local\`?**
- Standard users lack `SeCreateGlobalPrivilege`, causing `CreateFileMappingW` to fail with `ERROR_ACCESS_DENIED` in `Global\` namespace
- `Local\` namespace exists within the user's session, allowing standard user execution
- The Camera Frame Server (LOCAL SERVICE) runs in Session 0 but can access `Local\` handles created by the user-mode broker

**Creator-Consumer Pattern**:
1. **DLL Creates Handles**: `DirectPortClient.dll` loaded by Frame Server (LOCAL SERVICE) has privileges to create shared handles
2. **Permissive DACLs**: Security descriptor `D:P(A;;GA;;;AU)` grants access to all authenticated users
3. **User App Connects**: `VirtuaCam.exe` runs as standard user, opens existing handles via `OpenFileMappingW` instead of creating them

### Deployment Architecture

**Single-Folder Install** (`C:\Program Files\VirtuaCam\`):
- All binaries installed together (no split between Program Files and AppData)
- Installer requires admin elevation once during setup
- Runtime executables configured with `asInvoker` manifest level

**Split Runtime Permissions**:
- **Read-Only**: Binaries in `Program Files` (standard users can read/execute)
- **Read/Write**: Settings in `HKCU\Software\VirtuaCam` and `%LOCALAPPDATA%\VirtuaCam\`
- **No UAC Prompts**: After installation, users run the app without elevation requests

**Legacy Compatibility Fallback**:
- Modern Windows 11: Uses `MFCreateVirtualCamera` API (user-mode, no admin needed)
- Legacy Windows 10/older: Falls back to COM registration in HKLM (requires admin)
- Graceful prompt: If modern API fails, user is asked to elevate for legacy mode

## Technical Deep Dive

VirtuaCam's architecture relies on several key Windows technologies to achieve its high-performance, zero-copy pipeline:

1.  **Shared DirectX 11 Resources:**
    *   **Shared Texture:** A producer application creates an `ID3D11Texture2D` with the `D3D11_RESOURCE_MISC_SHARED_NTHANDLE` flag. This allows the texture's memory to be accessed by other processes on the same graphics adapter.
    *   **Shared Fence:** An `ID3D11Fence` is also created with the `D3D11_FENCE_FLAG_SHARED` flag. This synchronization primitive is used to signal when a new frame has been rendered to the shared texture, preventing the consumer from reading an incomplete frame.

2.  **Memory-Mapped Manifest File:**
    *   To enable discovery, each producer creates a memory-mapped file with a unique name (e.g., `DirectPort_Producer_Manifest_[ProcessID]`).
    *   This file contains a `BroadcastManifest` struct, which holds critical metadata: the dimensions and format of the shared texture, the LUID of the graphics adapter, and the global names of the shared texture and fence handles.

3.  **The Broker and Multiplexer (`DirectPortBroker.dll`):**
    *   At the heart of VirtuaCam is the broker. It runs in the background and is managed by the main `VirtuaCam.exe` controller.
    *   It continuously scans the system for producer manifest files (`Discovery.cpp`).
    *   When producers are found, the broker opens their shared resources (texture and fence).
    *   A **Multiplexer** (`Multiplexer.cpp`) is responsible for compositing frames from one or more producers into a single output texture. It can operate in two modes:
        *   **Single Source / Picture-in-Picture (PIP):** Renders a primary source fullscreen with smaller PIP overlays.
        *   **Grid Mode:** Arranges all discovered producers in an automatic grid layout.
    *   This final composited texture is then made available via its *own* shared texture and fence for the virtual camera driver to consume.

4.  **The Virtual Camera Media Source (`DirectPortClient.dll`):**
    *   This is the core COM DLL that registers itself with Windows as a Media Foundation virtual camera source.
    *   When an application like Teams requests a video frame, this DLL connects to the **Broker's** shared texture.
    *   It waits on the Broker's fence, copies the latest composited frame into the Media Foundation pipeline, and sends it to the requesting application. This final step is also a zero-copy GPU operation.

## Key Features

*   **High-Performance Zero-Copy Transfer:** Video frames are shared between processes entirely on the GPU using DirectX 11 shared resources, resulting in minimal latency and CPU overhead.
*   **Decoupled Architecture:** The virtual camera (consumer) and video-generating applications (producers) are separate processes. They can be started, stopped, and developed independently.
*   **Dynamic Producer Discovery:** The virtual camera automatically scans for and connects to any running, compatible producer application.
*   **Advanced Compositing:** A central broker multiplexes video from multiple sources. It can display a primary source with multiple Picture-in-Picture (PIP) overlays or create an automatic grid view of all available sources.
*   **System Tray Controller:** The camera's lifecycle is managed by a lightweight tray icon (`VirtuaCam.exe`), providing a professional user experience for selecting sources, managing PIP layouts, and accessing settings.
*   **Hardware-Accelerated Preview:** An on-demand preview window can be toggled from the tray menu to show the exact output of the camera, rendered with hardware acceleration.
*   **Multiple Producer Types:** Comes with pre-built producer modules for:
    *   Window/Screen Capture (`DirectPortMFGraphicsCapture.dll`)
    *   Physical Webcam Passthrough (`DirectPortMFCamera.dll`)
    *   Generic Consumer/Filter (`DirectPortConsumer.dll`)
*   **Modern C++ Implementation:** Built with C++20, plain COM, and the Windows Implementation Library (WIL) for stability and maintainability, using a modern CMake build system. The codebase deliberately avoids the C++/WinRT projection; the one WinRT API used (Windows.Graphics.Capture, for window capture) is accessed at the raw COM ABI level.

## How to Use VirtuaCam

Follow these steps to get the virtual camera up and running on your system.

### 1. Prerequisites & Dependencies

Before building, ensure you have the following installed:

*   **Visual Studio 2022** (or later) with the "Desktop development with C++" workload.
*   **Windows 10 SDK** (latest version recommended, usually installed with Visual Studio).
*   **Vcpkg** package manager.

#### Vcpkg Dependencies

This project requires one library that can be installed via vcpkg. Open your terminal and run the following command:

```sh
vcpkg install wil
```

The build script is pre-configured to find vcpkg in its default installation path (`C:\vcpkg`). If you have it installed elsewhere, you can specify the path when running the build script:

```powershell
.\build.ps1 -VcpkgRoot "C:\path\to\your\vcpkg"
```

### 2. Build the Project

With the prerequisites installed, you can now build the entire solution using the provided `build.ps1` PowerShell script. This will compile all necessary DLLs and EXEs.

```powershell
.\build.ps1
```

### 3. Register the Virtual Camera DLL (Administrator Required)

After a successful build, you must register the core COM server. This step requires Administrator privileges.

1.  Open **PowerShell** or **Command Prompt** as an **Administrator**.
2.  Navigate to the root directory of the VirtuaCam project where the build artifacts were copied.
3.  Run the following command:

    ```cmd
    regsvr32 DirectPortClient.dll
    ```

You should see a confirmation message that the DLL was registered successfully. You can also use the build script for this: `.\build.ps1 -Register`.

### 4. Run the VirtuaCam Controller

Double-click on `VirtuaCam.exe`. A new icon will appear in your system tray. This application runs the background broker process and provides the main user interface for controlling the camera.

### 5. Select a Video Source

Right-click the VirtuaCam tray icon to open the context menu.

*   **To share a window:** Go to `Source` -> `[Window Title]`. A producer process (`VirtuaCamProcess.exe`) will launch automatically to capture and broadcast that window's contents.
*   **To use a physical webcam:** Go to `Source` -> `[Webcam Name]`. A producer will launch to pass through your physical webcam feed.
*   **To use the auto-discovery grid:** Go to `Source` -> `Auto-Discovery Grid`. The camera will display a grid of all other active VirtuaCam-compatible producers running on your system.
*   **To add Picture-in-Picture:** Use the `Picture-in-Picture` sub-menu to select a source for the PIP overlay. You can enable additional PIP windows in the `Settings` menu.

### 6. Use in Your Target Application

Open an application like the **Windows Camera App**, **Zoom**, **Discord**, or **Microsoft Teams**. In the video settings, you should now be able to select **"VirtuaCam"** as your webcam. The feed you configured in the previous step will be displayed.

---

## Roadmap & Design Notes

*   [Integrating a user-mode virtual display driver (IddCx)](docs/VIRTUAL_DISPLAY_DRIVER.md) — expose a virtual monitor whose desktop feeds the camera.
*   [Running VirtuaCam as a Windows service](docs/WINDOWS_SERVICE.md) — boot-time camera availability with a non-elevated tray controller.

## Code Documentation Philosophy

VirtuaCam follows a **"Why, What, How"** documentation approach in source files:

### Header Comments (The "Why")
Every `.cpp` file starts with extensive header comments explaining:
- **Design decisions** and trade-offs considered
- **Security model** (e.g., `Local\` vs `Global\`, DACL choices)
- **Cross-process interaction patterns** (Creator-Consumer, handle tables)
- **Historical context** for non-obvious implementation choices

### Inline Comments (The "What" and "How")
Critical code sections include block comments that explain:
- **WHAT** the code does (especially for Windows API calls with subtle behavior)
- **HOW** it fits into the larger architecture
- **WHY** this specific approach was chosen over alternatives

### Example Patterns Documented
1. **VOM Handle Tables** (`Menu.cpp`): Explains deadlock prevention via generational handles
2. **Security Descriptors** (`Broker.cpp`): Documents SDDL string meaning and Frame Server access requirements
3. **Namespace Selection** (all IPC files): Justifies `Local\` over `Global\` for privilege avoidance
4. **Graceful Degradation** (`App.cpp`): Documents modern-vs-legacy API fallback flow

This documentation style ensures that future maintainers understand not just *what* the code does, but *why* it was written that way—preventing accidental reintroduction of solved problems.

## License

This project is licensed under the MIT License. See the `LICENSE` file for details.

## Acknowledgements

Thanks to the developer of the **[VCamSample](https://github.com/smourier/VCamSample)** project, which served as a valuable educational reference for Media Foundation virtual camera concepts during early development. The current Media Foundation source in this repository is an independent implementation written against the documented COM interfaces.
