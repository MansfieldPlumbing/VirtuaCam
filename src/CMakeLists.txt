# =============================================================================
# VirtuaCam
# =============================================================================
#   VirtuaCamCommon   static   shared-frame IPC, D3D11 helpers
#   VirtuaCamSource   DLL      Media Foundation virtual camera (loaded by the
#                              Windows Camera Frame Server; regsvr32 once)
#   VirtuaCam         EXE      tray app, broker/compositor, and -- when run
#                              with --producer -- the built-in producers
# =============================================================================

cmake_minimum_required(VERSION 3.21)
project(VirtuaCam LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- Windows Implementation Library (header only) ----------------------------
# Uses an installed package (e.g. vcpkg) when present, otherwise fetches the
# headers; nothing else is required beyond Visual Studio and the Windows SDK.
find_package(wil CONFIG QUIET)
if(NOT TARGET WIL::WIL)
    include(FetchContent)
    FetchContent_Declare(wil
        URL https://github.com/microsoft/wil/archive/refs/tags/v1.0.250325.1.tar.gz
        SOURCE_SUBDIR do-not-configure)
    FetchContent_MakeAvailable(wil)
    add_library(WIL::WIL INTERFACE IMPORTED)
    target_include_directories(WIL::WIL INTERFACE "${wil_SOURCE_DIR}/include")
endif()

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
endif()

# --- Common -----------------------------------------------------------------
add_library(VirtuaCamCommon STATIC
    Common/SharedFrame.cpp
    Common/Gpu.cpp)
target_include_directories(VirtuaCamCommon PUBLIC Common)
target_link_libraries(VirtuaCamCommon PUBLIC WIL::WIL d3d11 d3d12 dxgi d3dcompiler advapi32)

# --- Virtual camera media source --------------------------------------------
add_library(VirtuaCamSource SHARED
    Camera/CameraModule.cpp
    Camera/CameraSource.cpp
    Camera/CameraRenderer.cpp
    Camera/VirtuaCamSource.def)
target_link_libraries(VirtuaCamSource PRIVATE VirtuaCamCommon mfplat mfuuid mf mfsensorgroup runtimeobject)

# --- Tray application and producers -----------------------------------------
add_executable(VirtuaCam WIN32
    App/Main.cpp
    App/TrayApp.cpp
    App/Menu.cpp
    App/PreviewWindow.cpp
    App/Presenter.cpp
    App/Settings.cpp
    App/ProducerHost.cpp
    App/Broker.cpp
    App/Compositor.cpp
    App/Discovery.cpp
    App/ProducerMain.cpp
    App/CameraProducer.cpp
    App/CaptureProducer.cpp
    App/Whiteboard.cpp
    App/App.rc)
target_link_libraries(VirtuaCam PRIVATE VirtuaCamCommon
    mfplat mfuuid mf mfreadwrite mfsensorgroup
    d2d1 dcomp dwmapi uxtheme msimg32 comctl32 shcore shell32 user32 ole32 runtimeobject)
# The manifest (per-monitor DPI, common controls v6) comes from App.rc.
target_link_options(VirtuaCam PRIVATE /MANIFEST:NO)

# Keep everything the installer needs in one folder.
set_target_properties(VirtuaCamSource VirtuaCam PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

# --- Convenience targets (need an elevated shell) ----------------------------
add_custom_target(register_camera
    COMMAND regsvr32 /s "$<TARGET_FILE:VirtuaCamSource>"
    DEPENDS VirtuaCamSource
    COMMENT "Registering VirtuaCamSource.dll")
add_custom_target(unregister_camera
    COMMAND regsvr32 /u /s "$<TARGET_FILE:VirtuaCamSource>"
    COMMENT "Unregistering VirtuaCamSource.dll")
