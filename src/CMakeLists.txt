# =============================================================================
# CMakeLists.txt  --  VirtuaCam build definition
# =============================================================================
#
# Build targets (in dependency order):
#   VirtuaCamCommon         - Static library: shared utilities, GPU helpers, formats
#   DirectPortClient        - DLL: virtual camera COM object (registered via regsvr32)
#   DirectPortBroker        - DLL: GPU-compositing broker; composites producer frames
#   DirectPortMFCamera      - DLL: producer that captures from a physical webcam
#   DirectPortMFGraphicsCapture - DLL: producer that captures a window or the desktop
#   DirectPortConsumer      - DLL: example consumer/producer (reads broker, re-exports)
#   VirtuaCam               - EXE: system-tray controller app
#   VirtuaCamProcess        - EXE: thin launcher that loads a producer DLL in its own process
#
# Custom CMake targets:
#   register_vcam           - Run regsvr32 /s on DirectPortClient.dll  (needs Admin)
#   unregister_vcam         - Run regsvr32 /u /s on DirectPortClient.dll (needs Admin)
# =============================================================================

cmake_minimum_required(VERSION 3.20)
project(VirtuaCam LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# Use the static (non-DLL) CRT so binaries are self-contained.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- External dependencies (via vcpkg) ---
find_package(wil CONFIG REQUIRED)       # Windows Implementation Library: smart ptrs, HRESULT macros
set(VCAM_VCPKG_LIBS WIL::WIL)

# --- Windows SDK library groups ---
# Media Foundation: virtual camera source, MFT format conversion, device enumeration
set(VCAM_MF_LIBS mfplat mfuuid mfreadwrite mf)
# Direct3D / DXGI / D2D: GPU rendering, shared textures, fences
set(VCAM_D3D_LIBS d3d11 d2d1 dwrite windowscodecs d3d12 d3dcompiler dxgi)
# Core Windows: COM, shell, registry, audio, sync primitives.
# runtimeobject provides RoGetActivationFactory/RoInitialize for the raw-ABI
# Windows.Graphics.Capture access in DirectPortMFGraphicsCapture.
set(VCAM_WIN_LIBS ole32 oleaut32 runtimeobject propsys advapi32 user32 shell32 comctl32 shlwapi synchronization dwmapi avrt strmiids)

# =============================================================================
# VirtuaCamCommon  --  static library shared by all other targets
# =============================================================================
add_library(VirtuaCamCommon STATIC
    VirtuaCam/Tools.cpp         # String/GUID utilities, YUV conversion, registry helpers
    VirtuaCam/Guids.cpp         # COM GUID definitions (INITGUID must be defined here only)
    VirtuaCam/Formats.cpp       # Supported resolution + frame-rate combinations
    VirtuaCam/Discovery.cpp     # IPC: scans running processes for producer shared-memory manifests
    VirtuaCam/ShaderModule.cpp  # "Off mode" HLSL shader (retro CRT, shown when no source is live)
)
# Guids.cpp must be compiled without the precompiled header because it
# defines INITGUID, which must appear exactly once across the entire binary.
set_source_files_properties(VirtuaCam/Guids.cpp PROPERTIES SKIP_PRECOMPILE_HEADER ON)
target_link_libraries(VirtuaCamCommon PUBLIC ${VCAM_VCPKG_LIBS} ${VCAM_MF_LIBS} ${VCAM_D3D_LIBS} ${VCAM_WIN_LIBS})
target_compile_definitions(VirtuaCamCommon PRIVATE UNICODE _UNICODE)

# =============================================================================
# DirectPortClient  --  virtual camera COM DLL
# =============================================================================
# This is the DLL that Windows registers as a webcam via regsvr32.
# It implements IMFMediaSource2/IMFMediaStream2 and pulls frames from the broker.
add_library(DirectPortClient SHARED
    VirtuaCam/MFClient.cpp
    VirtuaCam/BrokerClient.cpp
    VirtuaCam/pch.cpp
    VirtuaCam/pch.h
)
set(DEF_FILE "${CMAKE_CURRENT_BINARY_DIR}/DirectPortClient.def")
file(WRITE ${DEF_FILE} "LIBRARY DirectPortClient\nEXPORTS\n    DllCanUnloadNow PRIVATE\n    DllGetClassObject PRIVATE\n    DllRegisterServer PRIVATE\n    DllUnregisterServer PRIVATE\n")
set_target_properties(DirectPortClient PROPERTIES
    OUTPUT_NAME "DirectPortClient"
    LINK_FLAGS "/DEF:${DEF_FILE}"
)
target_link_libraries(DirectPortClient PRIVATE VirtuaCamCommon)
target_compile_definitions(DirectPortClient PRIVATE _WINDOWS _USRDLL UNICODE _UNICODE)
target_precompile_headers(DirectPortClient PRIVATE VirtuaCam/pch.h)

# =============================================================================
# DirectPortBroker  --  GPU compositing broker DLL
# =============================================================================
# Loaded dynamically by VirtuaCam.exe at runtime.
# Owns the shared output texture+fence and runs the Multiplexer each frame.
add_library(DirectPortBroker SHARED
    VirtuaCam/Broker.cpp
    VirtuaCam/Multiplexer.cpp
)
set_target_properties(DirectPortBroker PROPERTIES OUTPUT_NAME "DirectPortBroker")
target_link_libraries(DirectPortBroker PRIVATE VirtuaCamCommon)
target_compile_definitions(DirectPortBroker PRIVATE _WINDOWS _USRDLL UNICODE _UNICODE)

# =============================================================================
# DirectPortMFCamera  --  physical webcam producer DLL
# =============================================================================
# Loaded by VirtuaCamProcess.exe with --type camera.
# Reads frames from a system camera via Media Foundation and publishes
# them to a named shared texture for the broker to composite.
add_library(DirectPortMFCamera SHARED
    VirtuaCam/MFCamera.cpp
    VirtuaCam/pch.cpp
)
set_target_properties(DirectPortMFCamera PROPERTIES OUTPUT_NAME "DirectPortMFCamera")
target_link_libraries(DirectPortMFCamera PRIVATE VirtuaCamCommon)
target_compile_definitions(DirectPortMFCamera PRIVATE _WINDOWS _USRDLL UNICODE _UNICODE PRODUCER_EXPORTS)
target_precompile_headers(DirectPortMFCamera PRIVATE VirtuaCam/pch.h)

# =============================================================================
# DirectPortMFGraphicsCapture  --  window/desktop capture producer DLL
# =============================================================================
# Loaded by VirtuaCamProcess.exe with --type capture.
# Uses the Windows.Graphics.Capture API to capture a target window (--hwnd)
# or the full desktop and publishes frames to a named shared texture.
add_library(DirectPortMFGraphicsCapture SHARED
    VirtuaCam/MFGraphicsCapture.cpp
    VirtuaCam/pch.cpp
)
set_target_properties(DirectPortMFGraphicsCapture PROPERTIES OUTPUT_NAME "DirectPortMFGraphicsCapture")
target_link_libraries(DirectPortMFGraphicsCapture PRIVATE VirtuaCamCommon)
target_compile_definitions(DirectPortMFGraphicsCapture PRIVATE _WINDOWS _USRDLL UNICODE _UNICODE PRODUCER_EXPORTS)
target_precompile_headers(DirectPortMFGraphicsCapture PRIVATE VirtuaCam/pch.h)

# =============================================================================
# DirectPortConsumer  --  example consumer/re-publisher DLL
# =============================================================================
# Loaded by VirtuaCamProcess.exe with --type consumer.
# Reads the broker's output texture, applies a shader transformation
# (colour inversion by default), and re-publishes the result as a new producer.
add_library(DirectPortConsumer SHARED
    VirtuaCam/Consumer.cpp
    VirtuaCam/pch.cpp
)
set_target_properties(DirectPortConsumer PROPERTIES OUTPUT_NAME "DirectPortConsumer")
target_link_libraries(DirectPortConsumer PRIVATE VirtuaCamCommon)
target_compile_definitions(DirectPortConsumer PRIVATE _WINDOWS _USRDLL UNICODE _UNICODE PRODUCER_EXPORTS)
target_precompile_headers(DirectPortConsumer PRIVATE VirtuaCam/pch.h)

# =============================================================================
# VirtuaCam  --  system-tray controller executable
# =============================================================================
# The main user-facing application.  Lives in the system tray and lets the
# user choose which source(s) feed the virtual camera.
add_executable(VirtuaCam WIN32
    VirtuaCam/App.cpp
    VirtuaCam/UI.cpp
    VirtuaCam/Menu.cpp
    VirtuaCam/WASAPI.cpp
    VirtuaCam/App.rc
)
set_target_properties(VirtuaCam PROPERTIES OUTPUT_NAME "VirtuaCam")
target_link_libraries(VirtuaCam PRIVATE VirtuaCamCommon)
target_compile_definitions(VirtuaCam PRIVATE UNICODE _UNICODE)
target_precompile_headers(VirtuaCam PRIVATE VirtuaCam/pch.h)

# =============================================================================
# VirtuaCamProcess  --  producer process launcher
# =============================================================================
# A minimal Win32 process that loads one producer DLL and runs its frame loop.
# Launched by VirtuaCam.exe with --type <camera|capture|consumer> [args...].
add_executable(VirtuaCamProcess WIN32
    VirtuaCam/Process.cpp
    VirtuaCam/Process.rc
)
set_target_properties(VirtuaCamProcess PROPERTIES OUTPUT_NAME "VirtuaCamProcess")
target_link_libraries(VirtuaCamProcess PRIVATE VirtuaCamCommon)
target_compile_definitions(VirtuaCamProcess PRIVATE UNICODE _UNICODE)

# =============================================================================
# COM registration helpers
# =============================================================================
add_custom_target(register_vcam
    COMMAND regsvr32 /s "$<TARGET_FILE:DirectPortClient>"
    DEPENDS DirectPortClient
    COMMENT "Registering DirectPortClient.dll as a virtual camera (requires Admin)..."
)
add_custom_target(unregister_vcam
    COMMAND regsvr32 /u /s "$<TARGET_FILE:DirectPortClient>"
    DEPENDS DirectPortClient
    COMMENT "Unregistering DirectPortClient.dll (requires Admin)..."
)
