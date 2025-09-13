cmake_minimum_required(VERSION 3.20)
project(VirtuaCam LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(wil CONFIG REQUIRED)
find_package(cppwinrt CONFIG REQUIRED)
set(VCAM_VCPKG_LIBS WIL::WIL Microsoft::CppWinRT )
set(VCAM_MF_LIBS mfplat mfuuid )

#-------------------------------------------------------------------------------
# 1. VirtuaCamSource (The Virtual Camera Media Source DLL)
#-------------------------------------------------------------------------------
set(VCAM_SOURCE_FILES
    DirectPortVirtuaCam/VirtualCamera.cpp
    DirectPortVirtuaCam/BrokerClient.cpp
    DirectPortVirtuaCam/Enumerator.cpp
    DirectPortVirtuaCam/Tools.cpp
    DirectPortVirtuaCam/pch.cpp 
    DirectPortVirtuaCam/pch.h
)
set(DEF_FILE "${CMAKE_CURRENT_BINARY_DIR}/DirectPortVirtuaCam.def")
file(WRITE ${DEF_FILE} "LIBRARY DirectPortVirtuaCam\nEXPORTS\n    DllCanUnloadNow PRIVATE\n    DllGetClassObject PRIVATE\n    DllRegisterServer PRIVATE\n    DllUnregisterServer PRIVATE\n")
add_library(VirtuaCamSource SHARED ${VCAM_SOURCE_FILES})
set_target_properties(VirtuaCamSource PROPERTIES OUTPUT_NAME "DirectPortVirtuaCam" LINK_FLAGS "/DEF:\"${DEF_FILE}\"")
target_link_libraries(VirtuaCamSource PRIVATE
    ${VCAM_VCPKG_LIBS} ${VCAM_MF_LIBS} strmiids d3d11 d2d1 dwrite windowscodecs ole32
    oleaut32 runtimeobject propsys d3d12 d3dcompiler
)
target_compile_definitions(VirtuaCamSource PRIVATE _WINDOWS _USRDLL UNICODE _UNICODE)

#-------------------------------------------------------------------------------
# 2. VirtuaCam (The Controlling Executable with Preview)
#-------------------------------------------------------------------------------
set(VCAM_APP_FILES 
    DirectPortVirtuaCam/App.cpp 
    DirectPortVirtuaCam/App.rc
)
add_executable(VirtuaCam WIN32 ${VCAM_APP_FILES})
set_target_properties(VirtuaCam PROPERTIES OUTPUT_NAME "VirtuaCam")
target_link_libraries(VirtuaCam PRIVATE
    ${VCAM_VCPKG_LIBS} ${VCAM_MF_LIBS} mfsensorgroup comctl32 runtimeobject
    d3d11 dxgi d3dcompiler
)
target_compile_definitions(VirtuaCam PRIVATE UNICODE _UNICODE)

#-------------------------------------------------------------------------------
# 3. DirectPortBroker (The Simplified Broker DLL)
#-------------------------------------------------------------------------------
set(BROKER_SOURCE_FILES DirectPortVirtuaCam/Broker.cpp)
add_library(VirtuaCastBroker SHARED ${BROKER_SOURCE_FILES})
set_target_properties(VirtuaCastBroker PROPERTIES OUTPUT_NAME "DirectPortBroker")
target_link_libraries(VirtuaCastBroker PRIVATE
    ${VCAM_VCPKG_LIBS} d3d11 dxgi advapi32 d3d12 d3dcompiler
)
target_compile_definitions(VirtuaCastBroker PRIVATE _WINDOWS _USRDLL UNICODE _UNICODE)

#-------------------------------------------------------------------------------
# 4. Custom Targets for COM Registration
#-------------------------------------------------------------------------------
add_custom_target(register_vcam COMMAND regsvr32 /s "$<TARGET_FILE:VirtuaCamSource>"
    DEPENDS VirtuaCamSource COMMENT "Registering DirectPortVirtuaCam.dll (requires Admin privileges)..."
)
add_custom_target(unregister_vcam COMMAND regsvr32 /u /s "$<TARGET_FILE:VirtuaCamSource>"
    DEPENDS VirtuaCamSource COMMENT "Unregistering DirectPortVirtuaCam.dll (requires Admin privileges)..."
)