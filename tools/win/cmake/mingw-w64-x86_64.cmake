# Cross-compile Opal for Windows on macOS/Linux with MinGW-w64 (brew install mingw-w64).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# Windows 10+ API level for everything (XLink's win_usb, depthai)
# GCC 14 made old pointer-type warnings errors; XLink's win_usb.c (2021) trips them
set(CMAKE_C_FLAGS_INIT "-D_WIN32_WINNT=0x0A00 -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Wno-error=implicit-function-declaration")
# depthai (2022) relies on headers that libc++ includes transitively but libstdc++ does not
set(CMAKE_CXX_FLAGS_INIT "-D_WIN32_WINNT=0x0A00 -include stdexcept -include utility -include cstdint")
