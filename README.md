Apologies for the formatting error. Here is the corrected and properly formatted Markdown document for VirtuaCam.

***

# VirtuaCam

![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)![Platform: Windows 11](https://img.shields.io/badge/Platform-Windows_11-blue.svg)![Language: C++20](https://img.shields.io/badge/Language-C++20-orange.svg)

VirtuaCam is a modern, high-performance virtual camera for Windows built with a decoupled producer-consumer architecture. It enables low-latency, zero-copy video injection from external DirectX applications, games, or other video sources, exposing them as a standard webcam on your system for use in applications like Zoom, Microsoft Teams, OBS, Discord, and more.

## Core Concept: A High-Performance Video Broker

Unlike traditional virtual cameras that generate their own content, VirtuaCam acts as a high-performance transport system—a "broker"—that discovers and composites video feeds from other applications (producers). This is achieved directly on the GPU, avoiding costly memory transfers between the CPU and GPU, which results in minimal performance impact.

The data flow is designed for efficiency:

`[Your App (Producer)]` ---> `[Shared D3D11 Texture & Fence]` ---> `[VirtuaCam Broker (Consumer)]` ---> `[Zoom, Teams, etc.]`

This architecture is ideal for applications like game streaming, creative coding, real-time video filters, screen sharing, or any scenario where you need to pipe a custom, hardware-accelerated video stream into a standard camera feed.

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
*   **Modern C++ Implementation:** Built with C++20 and robust Windows libraries like WIL and C++/WinRT for stability and maintainability, using a modern CMake build system.

## How to Use VirtuaCam

Follow these steps to get the virtual camera up and running on your system.

### 1. Build the Project

First, build the entire solution using the provided `build.ps1` PowerShell script. This will compile all necessary DLLs and EXEs.

```powershell
.\build.ps1
```

### 2. Register the Virtual Camera DLL (Administrator Required)

After a successful build, you must register the core COM server. This step requires Administrator privileges.

1.  Open **PowerShell** or **Command Prompt** as an **Administrator**.
2.  Navigate to the root directory of the VirtuaCam project where the build artifacts were copied.
3.  Run the following command:

    ```cmd
    regsvr32 DirectPortClient.dll
    ```

You should see a confirmation message that the DLL was registered successfully. You can also use the build script for this: `.\build.ps1 -Register`.

### 3. Run the VirtuaCam Controller

Double-click on `VirtuaCam.exe`. A new icon will appear in your system tray. This application runs the background broker process and provides the main user interface for controlling the camera.

### 4. Select a Video Source

Right-click the VirtuaCam tray icon to open the context menu.

*   **To share a window:** Go to `Source` -> `[Window Title]`. A producer process (`VirtuaCamProcess.exe`) will launch automatically to capture and broadcast that window's contents.
*   **To use a physical webcam:** Go to `Source` -> `[Webcam Name]`. A producer will launch to pass through your physical webcam feed.
*   **To use the auto-discovery grid:** Go to `Source` -> `Auto-Discovery Grid`. The camera will display a grid of all other active VirtuaCam-compatible producers running on your system.
*   **To add Picture-in-Picture:** Use the `Picture-in-Picture` sub-menu to select a source for the PIP overlay. You can enable additional PIP windows in the `Settings` menu.

### 5. Use in Your Target Application

Open an application like the **Windows Camera App**, **Zoom**, **Discord**, or **Microsoft Teams**. In the video settings, you should now be able to select **"VirtuaCam"** as your webcam. The feed you configured in the previous step will be displayed.

---

## License

This project is licensed under the MIT License. See the `LICENSE` file for details.

## Acknowledgements

A special thank you to the developer of the **[VCamSample](https://github.com/smourier/VCamSample)** project. VCamSample provided an excellent and clear foundational example of a Media Foundation virtual camera. Its well-structured code served as an invaluable educational resource and a starting point for understanding the core concepts involved in this project.
