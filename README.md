# VirtuaCam
A modern C++ virtual camera for Windows featuring a decoupled producer-consumer architecture. It enables low-latency, zero-copy video injection from external DirectX applications using shared resources.

![alt text](https://img.shields.io/badge/License-MIT-yellow.svg)
![alt text](https://img.shields.io/badge/Platform-Windows-blue.svg)
![alt text](https://img.shields.io/badge/Language-C%2B%2B-orange.svg)

A modern C++ virtual camera for Windows featuring a decoupled producer-consumer architecture. It enables low-latency, zero-copy video injection from external DirectX applications using shared resources.

Core Concept

DirectPort VirtuaCam is not a self-contained camera that generates its own video. Instead, it acts as a high-performance transport system that exposes a video feed from another application as a standard webcam on your system.

This is achieved through a producer-consumer model that operates directly on the GPU, avoiding costly memory transfers between the CPU and GPU.

The data flow is as follows:

[Your App (Producer)] ---> [Shared D3D11 Texture & Fence] ---> [VirtuaCam DLL (Consumer)] ---> [Zoom, Teams, OBS, etc.]

This architecture is ideal for applications like game streaming, creative coding, real-time video filters, or any scenario where you need to pipe a custom, hardware-accelerated video stream into a standard camera feed with minimal performance impact.

Key Features

High-Performance Zero-Copy Transfer: Video frames are shared between processes entirely on the GPU using DirectX 11 shared resources. This results in minimal latency and CPU overhead.

Decoupled Architecture: The virtual camera (consumer) and your video-generating application (producer) are separate processes. They can be started, stopped, and developed independently.

Dynamic Producer Discovery: The virtual camera automatically scans for and connects to any running, compatible producer application. If the producer closes, the camera gracefully switches to a "No Signal" slate.

System Tray Controller: The camera's lifecycle is managed by a lightweight, unobtrusive tray icon, providing a professional user experience.

Hardware-Accelerated Preview: An on-demand preview window can be toggled from the tray menu to show the exact output of the camera, rendered with hardware acceleration.

Modern C++ Implementation: Built with modern C++ and robust Windows libraries like WIL and C++/WinRT for stability and maintainability.

Project Components

The solution is divided into three main parts:

DirectPortVirtuaCamDLL (The Consumer/Camera)
This is the core virtual camera source, implemented as a COM DLL. It registers itself as a Media Foundation device. Its FrameGenerator is responsible for finding a producer and copying the shared frames into the media pipeline.

DirectPortVirtuaCamEXE (The Controller)
A lightweight Win32 application that runs in the system tray. It allows the user to enable or disable the virtual camera and toggle the preview window. It communicates with the DLL via a custom COM interface.

Building the Project
Prerequisites

Visual Studio 2022 (or later) with the "Desktop development with C++" workload.

Windows 10 SDK (latest version recommended, usually installed with Visual Studio).

Required Libraries: The project uses modern C++ helper libraries like WIL and C++/WinRT. These should be acquirable via NuGet or included as submodules in a full repository setup.

Build Steps

Clone the repository.

Open the DirectPortVirtuaCam.sln solution file in Visual Studio.

Select the desired configuration (e.g., Release, x64).

Build the solution (Build > Build Solution). This will produce DirectPortVirtuaCamDLL.dll, DirectPortVirtuaCamEXE.exe, and example.exe.

How to Use

Register the DLL: After building, you must register the COM server. Open a command prompt as an Administrator and run:

regsvr32 "C:\path\to\your\build\folder\DirectPortVirtuaCamDLL.dll"

Run the Producer: Start the sample producer application, example.exe. It will display a window with a simple rendered scene.

Run the Controller: Start the tray controller, DirectPortVirtuaCamEXE.exe. A new icon will appear in your system tray.

Enable the Camera: Right-click the tray icon and select "Enable Virtual Camera".

Select in Application: Open an application like the Windows Camera App, Zoom, or Discord. In the video settings, you should now be able to select "DirectPort VirtuaCam" as your webcam. The feed from the example.exe window will be displayed.

To disable the camera, right-click the tray icon and select "Disable Virtual Camera" or "Exit".

For Developers: Creating Your Own Producer

To make your own application a source for DirectPort VirtuaCam, you must implement the producer protocol:

Create Shared D3D11 Resources:

Create an ID3D11Texture2D with the D3D11_RESOURCE_MISC_SHARED_NTHANDLE flag.

Create an ID3D11Fence with the D3D11_FENCE_FLAG_SHARED flag.

Generate sharable HANDLEs for both resources.

Create a Shared Manifest:

Create a memory-mapped file named DirectPort_Producer_Manifest_[YourProcessID].

Map a view of this file to a BroadcastManifest struct.

Populate the manifest with the texture dimensions, format, adapter LUID, and the string names of your shared resource handles.

Render Loop:

In your render loop, draw your scene to the shared texture.

After rendering, signal the shared fence with an incrementing frame value (ID3D11DeviceContext4::Signal).

Update the frameValue in the shared manifest to notify consumers that a new frame is ready.

Refer to example.cpp for a complete, working implementation.

License

This project is licensed under the MIT License.

Copyright (c) [2025] [https://github.com/MansfieldPlumbing]

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
