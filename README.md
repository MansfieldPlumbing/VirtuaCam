
***

# VirtuaCam
A modern C++ virtual camera for Windows featuring a decoupled, high-performance producer-consumer architecture. It enables low-latency, zero-copy video injection from external DirectX applications using shared resources.

![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)![Platform: Windows](https://img.shields.io/badge/Platform-Windows-blue.svg)![Language: C++](https://img.shields.io/badge/Language-C%2B%2B-orange.svg)

<p align="center">
  <img src="https://github.com/user-attachments/assets/414a477f-d998-41e5-9465-4cbc158bbc05" alt="VirtuaCam in action with a producer application" />
</p>

## Core Concept

VirtuaCam is not a self-contained camera that generates its own video. Instead, it acts as a high-performance transport system that exposes a video feed from another application as a standard webcam on your system.

This is achieved through a producer-consumer model that operates directly on the GPU, avoiding costly memory transfers between the CPU and GPU.

The data flow is as follows:
```
[Your App (Producer)] ---> [Shared D3D11/12 Texture & Fence] ---> [VirtuaCam (Consumer)] ---> [Zoom, Teams, OBS, etc.]
```
This architecture is ideal for applications like game streaming, creative coding, real-time video filters, or any scenario where you need to pipe a custom, hardware-accelerated video stream into a standard camera feed with minimal performance impact.

## Key Features

*   **High-Performance Zero-Copy Transfer**: Video frames are shared between processes entirely on the GPU using DirectX shared resources, resulting in minimal latency and CPU overhead.
*   **Decoupled Architecture**: The virtual camera (consumer) and your video-generating application (producer) are separate processes that can be started, stopped, and developed independently.
*   **Dynamic Producer Discovery**: The virtual camera automatically scans for and connects to any running, compatible producer application. If the producer closes, the camera gracefully switches to a "No Signal" slate.
*   **System Tray Controller**: A lightweight, unobtrusive tray icon manages the camera's lifecycle, providing a professional user experience.
*   **Hardware-Accelerated Preview**: An on-demand preview window can be toggled from the tray menu to show the exact output of the camera, rendered with hardware acceleration.
*   **Modern C++ Implementation**: Built with modern C++ and robust Windows libraries like WIL and C++/WinRT for stability and maintainability.

<p align="center">
  <img src="https://github.com/user-attachments/assets/f2e730f2-cbb6-4b13-9907-a3325037d40b" alt="VirtuaCam UI and Preview Window" width="800"/>
</p>

## Project Components

The solution is divided into three main parts:

*   **VirtuaCamSource (The Consumer/Camera)**
    This is the core virtual camera source, implemented as a COM DLL. It registers itself as a Media Foundation device. Its `FrameGenerator` is responsible for finding a producer and copying the shared frames into the media pipeline.

*   **VirtuaCam (The Controller)**
    A lightweight Win32 application that runs in the system tray. It allows the user to manage the virtual camera's sources and toggle the preview window.

*   **DirectPortBroker (The Broker)**
    A helper DLL that acts as an intermediary, managing connections and compositing video streams if multiple producers are active.

## Building the Project

### Prerequisites

*   Visual Studio 2022 (or later) with the "Desktop development with C++" workload.
*   Windows 10 SDK (latest version recommended).
*   [vcpkg](https://vcpkg.io/) with the required libraries installed (e.g., WIL, C++/WinRT).

### Build Steps

1.  Clone the repository.
2.  Run the provided `build.ps1` script to configure and build the solution.
3.  The compiled binaries will be placed in the appropriate output directory.

## How to Use

1.  **Register the DLL**: After building, you must register the COM server. Open a command prompt **as an Administrator** and run:
    ```bash
    regsvr32 "C:\path\to\your\build\folder\DirectPortVirtuaCam.dll"
    ```
2.  **Run a Producer**: Start a producer application. You can use any of the examples from the [DirectPort](https://github.com/MansfieldPlumbing/DirectPort) project.
3.  **Run the Controller**: Start the tray controller, `VirtuaCam.exe`. A camera icon will appear in your system tray.
4.  **Select in Application**: Open an application like the Windows Camera App, Zoom, or Discord. In the video settings, you should now be able to select **"VirtuaCam"** as your webcam.

---

## For Developers

### Creating Your Own Producer with DirectPort

To make your own application a source for VirtuaCam, you must implement the producer protocol. The easiest way to achieve this is by using our companion library, **[DirectPort](https://github.com/MansfieldPlumbing/DirectPort)**.

DirectPort is a unified C++ and Python library that handles all the low-level DirectX and GPU synchronization details, allowing you to create a producer in just a few lines of code.

### Creative Applications

Looking for a ready-to-use creative application that already works with VirtuaCam? Check out **[FaceOn](https://github.com/MansfieldPlumbing/FaceOn)**, a real-time face-swapping and effects studio built on the DirectPort library. It's a perfect example of a powerful producer that can feed its output directly into VirtuaCam.

## License

This project is licensed under the MIT License.

> Copyright (c) 2025 [MansfieldPlumbing](https://github.com/MansfieldPlumbing)
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
