cmake_minimum_required(VERSION 3.20)
project(VirtuaCam LANGUAGES CXX)

add_compile_definitions(_WIN32_WINNT=0x0A00)

set(CMAKE_SYSTEM_VERSION "10.0.22621.0")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(wil CONFIG REQUIRED)
find_package(cppwinrt CONFIG REQUIRED)
set(VCAM_VCPKG_LIBS WIL::WIL Microsoft::CppWinRT)
set(VCAM_MF_LIBS mfplat mfuuid mfreadwrite mf)

# --- Common Library ---
add_library(VirtuaCamCommon STATIC
    VirtuaCam/Utilities.cpp
    VirtuaCam/Guids.cpp
    VirtuaCam/Formats.cpp
)
target_link_libraries(VirtuaCamCommon PUBLIC ${VCAM_VCPKG_LIBS} ${VCAM_MF_LIBS} propsys d3d12 strmiids)
target_compile_definitions(VirtuaCamCommon PRIVATE UNICODE _UNICODE)
target_precompile_headers(VirtuaCamCommon PRIVATE VirtuaCam/pch.h)

# --- 1. Virtual Camera Client ---
set(CLIENT_SOURCE_FILES
    VirtuaCam/MFActivate.cpp
    VirtuaCam/MFSource.cpp
    VirtuaCam/MFStream.cpp
    VirtuaCam/BrokerClient.cpp
    VirtuaCam/pch.cpp
)
set(DEF_FILE "${CMAKE_CURRENT_BINARY_DIR}/DirectPortClient.def")
file(WRITE ${DEF_FILE} "LIBRARY DirectPortClient\nEXPORTS\n    DllCanUnloadNow PRIVATE\n    DllGetClassObject PRIVATE\n    DllRegisterServer PRIVATE\n    DllUnregisterServer PRIVATE\n")
add_library(DirectPortClient SHARED ${CLIENT_SOURCE_FILES})
set_target_properties(DirectPortClient PROPERTIES OUTPUT_NAME "DirectPortClient" LINK_FLAGS "/DEF:\"${DEF_FILE}\"")
target_link_libraries(DirectPortClient PRIVATE
    VirtuaCamCommon
    ${VCAM_VCPKG_LIBS} ${VCAM_MF_LIBS} strmiids d3d11 d2d1 dwrite windowscodecs ole32
    oleaut32 runtimeobject propsys d3d12 d3dcompiler
)
target_compile_definitions(DirectPortClient PRIVATE _WINDOWS _USRDLL UNICODE _UNICODE)
target_precompile_headers(DirectPortClient PRIVATE VirtuaCam/pch.h)

# --- 2. Manager Application ---
set(MANAGER_SOURCE_FILES
    VirtuaCam/App.cpp
    VirtuaCam/App.rc
    VirtuaCam/UI.cpp
    VirtuaCam/Menu.cpp
    VirtuaCam/WASAPI.cpp
    VirtuaCam/Discovery.cpp
    VirtuaCam/GraphicsCapture.cpp
)
add_executable(VirtuaCam WIN32 ${MANAGER_SOURCE_FILES})
set_target_properties(VirtuaCam PROPERTIES OUTPUT_NAME "VirtuaCam")
target_link_libraries(VirtuaCam PRIVATE
    VirtuaCamCommon
    ${VCAM_VCPKG_LIBS} ${VCAM_MF_LIBS} strmiids ole32 mfsensorgroup comctl32 runtimeobject
    d3d11 dxgi d3dcompiler avrt propsys d3d12 dwmapi windowsapp
)
target_compile_definitions(VirtuaCam PRIVATE UNICODE _UNICODE)
target_precompile_headers(VirtuaCam PRIVATE VirtuaCam/pch.h)

# --- 3. Broker Library ---
set(BROKER_SOURCE_FILES
    VirtuaCam/Broker.cpp
    VirtuaCam/Multiplexer.cpp
)
add_library(DirectPortBroker SHARED ${BROKER_SOURCE_FILES})
set_target_properties(DirectPortBroker PROPERTIES OUTPUT_NAME "DirectPortBroker")
target_link_libraries(DirectPortBroker PRIVATE
    VirtuaCamCommon
    ${VCAM_VCPKG_LIBS} d3d11 dxgi advapi32 d3d12 d3dcompiler propsys strmiids
)
target_compile_definitions(DirectPortBroker PRIVATE _WINDOWS _USRDLL UNICODE _UNICODE)

# --- 4. Hardware Camera Producer Library ---
set(MFCAMERA_SOURCE_FILES
    VirtuaCam/CameraModule.cpp
)
add_library(DirectPortMFCamera SHARED ${MFCAMERA_SOURCE_FILES})
set_target_properties(DirectPortMFCamera PROPERTIES OUTPUT_NAME "DirectPortMFCamera")
target_link_libraries(DirectPortMFCamera PRIVATE
    VirtuaCamCommon
    ${VCAM_VCPKG_LIBS} ${VCAM_MF_LIBS}
    d3d12 dxgi d3dcompiler advapi32 shlwapi synchronization dxguid strmiids
)
target_compile_definitions(DirectPortMFCamera PRIVATE _WINDOWS _USRDLL UNICODE _UNICODE CAMERAMODULE_EXPORTS)
target_precompile_headers(DirectPortMFCamera PRIVATE VirtuaCam/pch.h)

# --- 5. Graphics Capture Producer Library ---
set(MFCAPTURE_SOURCE_FILES
    VirtuaCam/GraphicsCapture.cpp
)
add_library(DirectPortMFCapture SHARED ${MFCAPTURE_SOURCE_FILES})
set_target_properties(DirectPortMFCapture PROPERTIES OUTPUT_NAME "DirectPortMFCapture")
target_link_libraries(DirectPortMFCapture PRIVATE
    VirtuaCamCommon
    ${VCAM_VCPKG_LIBS} d3d11 dxgi advapi32 d3d12 d3dcompiler dwmapi windowsapp
)
target_compile_definitions(DirectPortMFCapture PRIVATE _WINDOWS _USRDLL UNICODE _UNICODE MFCAPTURE_EXPORTS)
target_precompile_headers(DirectPortMFCapture PRIVATE VirtuaCam/pch.h)

# --- 6. Generic Worker Process ---
set(PROCESS_SOURCE_FILES
    VirtuaCam/Process.cpp
    VirtuaCam/Multiplexer.cpp
    VirtuaCam/Process.rc
)
add_executable(VirtuaCamProcess WIN32 ${PROCESS_SOURCE_FILES})
set_target_properties(VirtuaCamProcess PROPERTIES OUTPUT_NAME "VirtuaCamProcess")
target_link_libraries(VirtuaCamProcess PRIVATE
    VirtuaCamCommon
    ${VCAM_VCPKG_LIBS} ${VCAM_MF_LIBS} d3d11 d3d12 dxgi advapi32 d3dcompiler windowsapp strmiids
)
target_compile_definitions(VirtuaCamProcess PRIVATE UNICODE _UNICODE)
target_precompile_headers(VirtuaCamProcess PRIVATE VirtuaCam/pch.h)

# --- 7. Consumer Library ---
set(CONSUMER_SOURCE_FILES
    VirtuaCam/Consumer.cpp
)
add_library(DirectPortConsumer SHARED ${CONSUMER_SOURCE_FILES})
set_target_properties(DirectPortConsumer PROPERTIES OUTPUT_NAME "DirectPortConsumer")
target_link_libraries(DirectPortConsumer PRIVATE
    VirtuaCamCommon
    ${VCAM_VCPKG_LIBS} d3d11 dxgi d3dcompiler advapi32
)
target_compile_definitions(DirectPortConsumer PRIVATE _WINDOWS _USRDLL UNICODE _UNICODE CONSUMER_EXPORTS)
target_precompile_headers(DirectPortConsumer PRIVATE VirtuaCam/pch.h)

# --- Registration Targets ---
add_custom_target(register_vcam COMMAND regsvr32 /s "$<TARGET_FILE:DirectPortClient>"
    DEPENDS DirectPortClient COMMENT "Registering DirectPortClient.dll (requires Admin privileges)..."
)
add_custom_target(unregister_vcam COMMAND regsvr32 /u /s "$<TARGET_FILE:DirectPortClient>"
    DEPENDS DirectPortClient COMMENT "Unregistering DirectPortClient.dll (requires Admin privileges)..."
)