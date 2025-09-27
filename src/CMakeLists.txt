cmake_minimum_required(VERSION 3.20)
project(VirtuaCam LANGUAGES CXX)
# set(CMAKE_SYSTEM_VERSION "10.0.22621.0")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(wil CONFIG REQUIRED)
find_package(cppwinrt CONFIG REQUIRED)
set(VCAM_VCPKG_LIBS WIL::WIL Microsoft::CppWinRT)
set(VCAM_MF_LIBS mfplat mfuuid mfreadwrite mf)
set(VCAM_D3D_LIBS d3d11 d2d1 dwrite windowscodecs d3d12 d3dcompiler dxgi)
set(VCAM_WIN_LIBS ole32 oleaut32 runtimeobject propsys advapi32 user32 shell32 comctl32 shlwapi synchronization dwmapi windowsapp avrt strmiids)

add_library(VirtuaCamCommon STATIC
    VirtuaCam/Tools.cpp
    VirtuaCam/Guids.cpp
    VirtuaCam/Enumerator.cpp
    VirtuaCam/Formats.cpp
    VirtuaCam/Discovery.cpp
    VirtuaCam/ShaderModule.cpp
)
set_source_files_properties(VirtuaCam/Guids.cpp PROPERTIES SKIP_PRECOMPILE_HEADER ON)
target_link_libraries(VirtuaCamCommon PUBLIC ${VCAM_VCPKG_LIBS} ${VCAM_MF_LIBS} ${VCAM_D3D_LIBS} ${VCAM_WIN_LIBS})
target_compile_definitions(VirtuaCamCommon PRIVATE UNICODE _UNICODE)

add_library(DirectPortClient SHARED
    VirtuaCam/VirtualCamera.cpp
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

add_library(DirectPortBroker SHARED
    VirtuaCam/Broker.cpp
    VirtuaCam/Multiplexer.cpp
)
set_target_properties(DirectPortBroker PROPERTIES OUTPUT_NAME "DirectPortBroker")
target_link_libraries(DirectPortBroker PRIVATE VirtuaCamCommon)
target_compile_definitions(DirectPortBroker PRIVATE _WINDOWS _USRDLL UNICODE _UNICODE)

add_library(DirectPortMFCamera SHARED 
    VirtuaCam/CameraModule.cpp
    VirtuaCam/pch.cpp
)
set_target_properties(DirectPortMFCamera PROPERTIES OUTPUT_NAME "DirectPortMFCamera")
target_link_libraries(DirectPortMFCamera PRIVATE VirtuaCamCommon)
target_compile_definitions(DirectPortMFCamera PRIVATE _WINDOWS _USRDLL UNICODE _UNICODE PRODUCER_EXPORTS)
target_precompile_headers(DirectPortMFCamera PRIVATE VirtuaCam/pch.h)

add_library(DirectPortMFGraphicsCapture SHARED 
    VirtuaCam/GraphicsCapture.cpp
    VirtuaCam/pch.cpp 
)
set_target_properties(DirectPortMFGraphicsCapture PROPERTIES OUTPUT_NAME "DirectPortMFGraphicsCapture")
target_link_libraries(DirectPortMFGraphicsCapture PRIVATE VirtuaCamCommon)
target_compile_definitions(DirectPortMFGraphicsCapture PRIVATE _WINDOWS _USRDLL UNICODE _UNICODE PRODUCER_EXPORTS)
target_precompile_headers(DirectPortMFGraphicsCapture PRIVATE VirtuaCam/pch.h)

add_library(DirectPortConsumer SHARED 
    VirtuaCam/Consumer.cpp
    VirtuaCam/pch.cpp
)
set_target_properties(DirectPortConsumer PROPERTIES OUTPUT_NAME "DirectPortConsumer")
target_link_libraries(DirectPortConsumer PRIVATE VirtuaCamCommon)
target_compile_definitions(DirectPortConsumer PRIVATE _WINDOWS _USRDLL UNICODE _UNICODE PRODUCER_EXPORTS)
target_precompile_headers(DirectPortConsumer PRIVATE VirtuaCam/pch.h)


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

add_executable(VirtuaCamProcess WIN32
    VirtuaCam/Process.cpp
    VirtuaCam/Process.rc
)
set_target_properties(VirtuaCamProcess PROPERTIES OUTPUT_NAME "VirtuaCamProcess")
target_link_libraries(VirtuaCamProcess PRIVATE VirtuaCamCommon)
target_compile_definitions(VirtuaCamProcess PRIVATE UNICODE _UNICODE)

add_custom_target(register_vcam COMMAND regsvr32 /s "$<TARGET_FILE:DirectPortClient>"
    DEPENDS DirectPortClient
    COMMENT "Registering DirectPortClient.dll (requires Admin privileges)..."
)
add_custom_target(unregister_vcam COMMAND regsvr32 /u /s "$<TARGET_FILE:DirectPortClient>"
    DEPENDS DirectPortClient
    COMMENT "Unregistering DirectPortClient.dll (requires Admin privileges)..."
)