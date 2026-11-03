# "Opal C1" Windows 11 virtual camera (Media Foundation MFCreateVirtualCamera).
#   OpalVirtualCamera.dll - media source COM server loaded by the Camera Frame Server service
#   OpalVCamTest.exe      - registers the camera for the session and feeds it a test pattern
#
# Real target: MSVC (VS 2022) + Windows SDK 10.0.22621 or newer, x64:
#   cmake -S tools/win/vcam -B build-vcam -A x64 && cmake --build build-vcam --config Release
# Also works via add_subdirectory(vcam) from a top-level tools/win/CMakeLists.txt.
#
# Cross-compile check on macOS (MinGW-w64; not shipped, only proves the code compiles and links):
#   cmake -S tools/win/vcam -B tools/vendor/vcam-mingw -G Ninja -DCMAKE_SYSTEM_NAME=Windows \
#     -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
#     && cmake --build tools/vendor/vcam-mingw
cmake_minimum_required(VERSION 3.16)
project(opal_vcam CXX)

if(NOT WIN32)
  message(FATAL_ERROR "opal_vcam targets Windows; cross-compile with CMAKE_SYSTEM_NAME=Windows")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows 11 21H2 (NTDDI_WIN10_CO) API surface: the SDK only declares MFCreateVirtualCamera / IMFVirtualCamera at this
# level. NTDDI_VERSION must be explicit: deriving it from _WIN32_WINNT=0x0A00 would give plain Windows 10 (1507) and
# hide IMFMediaStream2, IMFSampleAllocatorControl, the sensor-profile API, etc.
set(OPAL_VCAM_DEFINES WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A00000B)

set(OPAL_VCAM_LIBS mfplat mfuuid ole32 oleaut32 advapi32 uuid)

add_library(OpalVirtualCamera SHARED
  src/dllmain.cpp
  src/activator.cpp
  src/media_source.cpp
  src/media_stream.cpp
  src/frame_channel.cpp
  OpalVirtualCamera.def
  src/version.rc)
target_compile_definitions(OpalVirtualCamera PRIVATE ${OPAL_VCAM_DEFINES})
target_link_libraries(OpalVirtualCamera PRIVATE ${OPAL_VCAM_LIBS})
set_target_properties(OpalVirtualCamera PROPERTIES PREFIX "")  # MinGW would otherwise name it libOpalVirtualCamera.dll

add_executable(OpalVCamTest test/vcamtest.cpp)
target_compile_definitions(OpalVCamTest PRIVATE ${OPAL_VCAM_DEFINES})
target_link_libraries(OpalVCamTest PRIVATE ${OPAL_VCAM_LIBS})

if(MSVC)
  target_compile_options(OpalVirtualCamera PRIVATE /W4 /permissive-)
  target_compile_options(OpalVCamTest PRIVATE /W4 /permissive-)
  # Static CRT: the DLL is loaded into the Frame Server service, which shouldn't depend on a VC++ redistributable.
  set_property(TARGET OpalVirtualCamera OpalVCamTest PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  target_compile_options(OpalVirtualCamera PRIVATE -Wall -Wextra)
  target_compile_options(OpalVCamTest PRIVATE -Wall -Wextra)
  # MinGW's ks.h declares KS GUIDs such as PINNAME_VIDEO_CAPTURE extern (MSVC resolves them via __uuidof).
  target_link_libraries(OpalVirtualCamera PRIVATE ksguid)
  # Fully static runtime (libgcc, libstdc++, winpthread) so the check build has no MinGW DLL dependencies.
  target_link_options(OpalVirtualCamera PRIVATE -static)
  target_link_options(OpalVCamTest PRIVATE -static)
endif()
