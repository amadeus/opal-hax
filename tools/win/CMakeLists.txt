# Opal for Windows. Built by build-win.ps1 (which first builds depthai-core with Opal's firmware); by hand:
#   cmake -S tools/win -B build -G "Visual Studio 17 2022" -A x64 -Ddepthai_DIR=<depthai install>/lib/cmake/depthai
#   cmake --build build --config Release
# Outputs Opal.exe (tray app), OpalVirtualCamera.dll + OpalVCamTest.exe (virtual camera, see vcam/).
cmake_minimum_required(VERSION 3.20)
project(OpalForWindows C CXX)

if(NOT WIN32)
  message(FATAL_ERROR "Opal for Windows builds on Windows (MSVC). The macOS test tools are in smoke/.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
if(MSVC)
  add_compile_options(/utf-8 /W3 /MP)
endif()
# Windows 11 API level (MFCreateVirtualCamera is only declared from NTDDI_WIN10_CO)
set(OPAL_WIN_DEFINES WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A00000B)

find_package(depthai CONFIG REQUIRED)

# Dear ImGui (no CMake build of its own)
include(FetchContent)
FetchContent_Declare(imgui URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.5.tar.gz SOURCE_SUBDIR none)
FetchContent_MakeAvailable(imgui)
# FreeType renders the UI text (hinted, closer to Windows' own text than ImGui's built-in rasterizer); no optional deps
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
FetchContent_Declare(freetype URL https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz
  URL_HASH SHA256=0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289)
# depthai's package config leaves ZLIB_FOUND set, which makes FreeType expect a system zlib; it has its own copy
unset(ZLIB_FOUND)
FetchContent_MakeAvailable(freetype)
add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/misc/freetype/imgui_freetype.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/freetype)
target_compile_definitions(imgui PUBLIC IMGUI_ENABLE_FREETYPE PRIVATE ${OPAL_WIN_DEFINES})
target_link_libraries(imgui PRIVATE freetype)

# camera core (portable; also built on macOS by smoke/)
add_library(opalcore STATIC core/opal_pipeline.cpp core/opal_camera.cpp)
target_link_libraries(opalcore PUBLIC depthai::core)
target_compile_definitions(opalcore PRIVATE ${OPAL_WIN_DEFINES})

# tray app
add_executable(Opal WIN32
  app/main.cpp app/ui.cpp app/fluent.cpp app/effects.cpp app/overlay.cpp app/settings.cpp app/vcam_producer.cpp app/installer.cpp app/opal.rc)
target_compile_definitions(Opal PRIVATE ${OPAL_WIN_DEFINES})
target_link_libraries(Opal PRIVATE opalcore imgui d3d11 dxgi d3dcompiler d2d1 dwrite windowscodecs mfplat mfuuid
  shell32 comdlg32 ole32 oleaut32 advapi32 user32 dwmapi cfgmgr32 uuid)
set_target_properties(Opal PROPERTIES OUTPUT_NAME "Opal")
if(NOT MSVC)
  # wWinMain entry (MSVC picks it automatically); static GCC runtime so the exe needs no MinGW DLLs
  target_link_options(Opal PRIVATE -municode -static -static-libgcc -static-libstdc++)
  # XLink's link interface ends in -Wl,-Bdynamic; the standard libraries go last on the link line, so switch back
  # there and the C++ runtime and pthreads link statically too
  set(CMAKE_CXX_STANDARD_LIBRARIES "-Wl,-Bstatic -lstdc++ -lwinpthread ${CMAKE_CXX_STANDARD_LIBRARIES}")
endif()

# OpalWebcamTest.exe: diagnostic for the camera's flashed webcam program (see webcamtest/webcamtest.cpp)
add_executable(OpalWebcamTest webcamtest/webcamtest.cpp)
target_compile_definitions(OpalWebcamTest PRIVATE ${OPAL_WIN_DEFINES})
target_link_libraries(OpalWebcamTest PRIVATE depthai::core shell32)
if(NOT MSVC)
  target_link_options(OpalWebcamTest PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# virtual camera
add_subdirectory(vcam)

# dist/: what goes to the PC (Opal.exe installs itself; OpalVCamTest.exe stays out, it's a debugging tool)
set(OPAL_DIST ${CMAKE_CURRENT_SOURCE_DIR}/dist)
add_custom_target(dist ALL
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${OPAL_DIST}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${OPAL_DIST}
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:Opal> $<TARGET_FILE:OpalVirtualCamera> ${OPAL_DIST}
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/opal-template ${OPAL_DIST}/resources/opal-template
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/app/README.txt ${OPAL_DIST}
  DEPENDS Opal OpalVirtualCamera OpalVCamTest
  COMMENT "Assembling dist/")
